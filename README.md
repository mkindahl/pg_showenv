# Show environment variables for a backend

A PostgreSQL extension that shows the server process's environment variables as a SQL result set.

This extension can be used to demonstrate that secrets injected into the environment of the PostgreSQL server process can be read by any database superuser via a simple `SELECT`.

## Build and install

Requires PostgreSQL server headers (`postgresql-server-dev-*` on Debian/Ubuntu, `postgresql@N` on Homebrew).

```sh
make
make install
```

## Usage

```sql
-- This requires either superuser privileges, or that the extension is trusted
CREATE EXTENSION showenv;

-- Show every environment variable the server process inherited
SELECT name, value FROM environment_variables();

-- Filter to secrets
SELECT name, value FROM environment_variables() WHERE name ILIKE '%password%' OR name ILIKE '%secret%' OR name ILIKE '%token%';
```

The installation script revokes `EXECUTE` privileges, but if that is not done,
the user can call this function to read all environment variables, including
secrets.

If superuser grants `EXECUTE` privileges to non-superusers (or is tricked into
doing that), the user can read all environment variables, including secrets.

## Exposing environment variables

The attack surface is any secret that an operator injects into the PostgreSQL server's OS environment before starting the process. Common examples:

- `POSTGRES_PASSWORD` (used by the official Docker image and many init scripts)
- `AWS_SECRET_ACCESS_KEY` set on the system service
- `DATABASE_URL` containing credentials in the connection string
- Arbitrary secrets passed via `Environment=` in a systemd unit or equivalent

> [!NOTE]
> The postgres server installed using Homebrew or the postgres server started
> using systemd does not consume any specific environment variable for
> authentication. The variable name below (`MY_SECRET`) is therefore arbitrary;
> the point of the demo is that **any** variable the server process inherits
> becomes readable via SQL, regardless of whether PostgreSQL itself attaches
> meaning to it.

### On macOS (Homebrew / launchd)


```sh
# 1. Inject a secret into launchd's environment — picked up by services started after this
launchctl setenv MY_SECRET "hunter2"

# 2. Restart PostgreSQL so it inherits the variable
brew services restart postgresql@18

# 3. Connect and read it back via SQL
psql -U postgres -c "
  CREATE EXTENSION IF NOT EXISTS showenv;
  SELECT name, value FROM environment_variables() WHERE name = 'MY_SECRET';
"
#    name    |  value
# -----------+---------
#  MY_SECRET | hunter2

# 4. Clean up
launchctl unsetenv MY_SECRET
brew services restart postgresql@18
```

### On Linux (systemd)

```sh
# 1. Add the secret to the PostgreSQL service's environment
sudo systemctl edit postgresql   # or postgresql@16, postgresql-16, etc.
```

In the override file that opens:

```ini
[Service]
Environment="MY_SECRET=hunter2"
```

```sh
# 2. Reload and restart
sudo systemctl daemon-reload
sudo systemctl restart postgresql

# 3. Connect and read it back via SQL
psql -U postgres -c "
  CREATE EXTENSION IF NOT EXISTS showenv;
  SELECT name, value FROM environment_variables() WHERE name = 'MY_SECRET';
"
#    name    |  value
# -----------+---------
#  MY_SECRET | hunter2
```

### On Docker (PGDG official image)

The easiest way to demonstrate this attack. The official `postgres` image runs as an unprivileged user and inherits all `docker run -e` variables.

The repository includes a [`Dockerfile.demo`](Dockerfile.demo) that builds the extension on top of `postgres:18`, and a [`docker-compose.yml`](docker-compose.yml) that wires it up with a few example secrets in the environment.

#### Quick demo with `docker run`

```sh
# Build the image using the Dockerfile.demo in this repo
docker build -f Dockerfile.demo -t postgres-showenv:18 .

# Start the container in the background with a secret in the environment
docker run -d --name postgres-showenv \
  -e POSTGRES_PASSWORD=hunter2 \
  -e POSTGRES_USER=postgres \
  postgres-showenv:18

# Run the SQL that reads the secret back out of the server process
# (give the server a couple of seconds to initialise on first start)
docker exec postgres-showenv psql -U postgres -c "
  CREATE EXTENSION showenv;
  SELECT name, value FROM environment_variables() WHERE name = 'POSTGRES_PASSWORD';
"
#        name        |  value
# -------------------+---------
#  POSTGRES_PASSWORD | hunter2

# Stop and remove the container
docker stop postgres-showenv
docker rm postgres-showenv
```

#### Docker Compose demo

The shipped [`docker-compose.yml`](docker-compose.yml) builds from `Dockerfile.demo` and injects `POSTGRES_PASSWORD`, `API_KEY`, `DATABASE_URL`, and `DB_ENCRYPTION_KEY` into the server's environment so you can see them all leak at once:

```sh
docker-compose up -d
docker-compose exec postgres psql -U postgres -c "
  CREATE EXTENSION showenv;
  SELECT name, value FROM environment_variables()
  WHERE name IN ('POSTGRES_PASSWORD', 'API_KEY', 'DATABASE_URL', 'DB_ENCRYPTION_KEY')
  ORDER BY name;
"
#        name        |              value
# -------------------+----------------------------------
#  API_KEY           | sk-abcdef123456
#  DATABASE_URL      | postgres://user:password@localhost/db
#  DB_ENCRYPTION_KEY | 0x1a2b3c4d5e6f7b000000000000000000
#  POSTGRES_PASSWORD | hunter2

docker-compose down
```

#### Built-in variables exposed by the `postgres:18` image

The official PGDG image automatically sets these environment variables, all of which are exposed:

| Variable            | Example                         | Note                                     |
|---------------------|---------------------------------|------------------------------------------|
| `POSTGRES_PASSWORD` | `hunter2`                       | **Superuser password — the main secret** |
| `POSTGRES_USER`     | `postgres`                      | Superuser name                           |
| `POSTGRES_DB`       | `postgres`                      | Default database                         |
| `PG_VERSION`        | `18.4-1.pgdg13+1`               | PostgreSQL version string                |
| `PGDATA`            | `/var/lib/postgresql/18/docker` | Data directory path                      |
| `PATH`              | `…:/usr/lib/postgresql/18/bin`  | Includes PostgreSQL bin directory        |

Any additional variables passed via `docker run -e` or `docker-compose` environment section are also exposed, including API keys, database URLs with embedded credentials, and encryption keys.

### Why PGPASSWORD doesn't appear

`PGPASSWORD` is consumed by the `libpq` client library before the TCP connection is made. The server process never sees it. The variables that matter are those set in the **server's** environment, not the client's.

## Is SECURITY DEFINER required?

**No.** The function exposes environment variables regardless of whether `SECURITY DEFINER` is present. The C code directly accesses `extern char **environ`, which is available to any code running in the PostgreSQL server process.

`SECURITY DEFINER` only affects SQL-level permissions — it allows non-superusers to call a function with the privileges of the user who created it. But in this extension, the real gate is the `REVOKE ALL ON FUNCTION … FROM PUBLIC` statement, which prevents all non-superusers from calling the function at all.

**Why the attack works without SECURITY DEFINER:**
- A superuser calling the function reads `environ` directly — `SECURITY DEFINER` is irrelevant.
- Non-superusers cannot call the function because of the `REVOKE` statement, regardless of `SECURITY DEFINER`.
- The only scenario where `SECURITY DEFINER` matters is if you created a *wrapper* function marked `SECURITY DEFINER` that calls `environment_variables()` and granted execute to non-superusers — an unlikely design mistake.

## Supply-chain attack vector

> [!NOTE]
> **This extension is not itself a supply-chain attack, but it demonstrates a technique that could be embedded in one.**

A trusted, widely-used PostgreSQL extension could be compromised and updated with environment-variable exfiltration code. Users would:
1. Trust the established extension and skip code review
2. Install the update via their package manager
3. Have the malicious C code run with superuser privileges on every server restart
4. Leak `POSTGRES_PASSWORD`, `AWS_SECRET_ACCESS_KEY`, and other secrets to an attacker

**Why this is dangerous:**
- Extensions run as the PostgreSQL superuser (full OS environment access)
- C code exfiltration is invisible to SQL-level auditing
- One compromised extension affects many downstream deployments
- Real precedent: `xz-utils` backdoor (2024), `event-stream` npm (2018)

See [Mitigations](#mitigations) below — particularly *Fundamental* (don't put secrets in environment variables in the first place) and *Extension security* (review, pin, and audit the C extensions you install).

## Security implications

- **Any superuser can read all secrets.** If a superuser account is compromised, all env-var secrets are exposed. This is the primary direct attack vector.

- **Non-superuser escalation via `GRANT`.** A superuser can grant `EXECUTE` permission to non-superusers, intentionally or accidentally. Once granted, those users can read environment variables.

- **Trusted extension compromise.** A compromised trusted PostgreSQL extension can read environment variables via C code, invisible to SQL-level auditing. This is a viable supply-chain attack.

- **No server restart required.** Once the extension is installed, secrets are readable immediately via SQL.

- **SECURITY DEFINER wrapper risk.** See [Is SECURITY DEFINER required?](#is-security-definer-required) above — a wrapper marked `SECURITY DEFINER` and granted to non-superusers would extend the blast radius beyond just superusers.

## Mitigations

### Fundamental

Do not pass secrets as environment variables to the PostgreSQL server process.
- Use a secrets manager (AWS Secrets Manager, HashiCorp Vault, etc.) and fetch credentials at application startup instead.
- Use `pg_hba.conf` peer or cert authentication rather than password-based auth where possible.

### File-mounted secrets

Mounting a secret as a file instead of injecting it as an environment variable closes the specific attack this extension demonstrates. The secret never enters `environ`, so `SELECT * FROM environment_variables()` cannot leak it.

In Kubernetes, prefer a `volumeMounts` projection of a `Secret` over `env`/`envFrom`:

```yaml
# Vulnerable: secret becomes an env var, readable by showenv
env:
  - name: POSTGRES_PASSWORD
    valueFrom:
      secretKeyRef: { name: pg-secret, key: password }

# Better: secret is mounted as a tmpfs file, not visible to environ
volumes:
  - name: pg-secret
    secret: { secretName: pg-secret }
volumeMounts:
  - { name: pg-secret, mountPath: /etc/postgres-secret, readOnly: true }
```

Equivalent patterns exist for Docker Swarm (`secrets:` in compose, mounted at `/run/secrets/`) and HashiCorp Vault Agent (template-rendered files).

**Caveat — this is defense in depth, not a complete fix.** A malicious C extension runs as the PostgreSQL OS process and can still `open()` and `read()` any file that process can read, including the mounted secret. The exfiltration code is one line longer than this extension's, not fundamentally different. File-mounted secrets raise the discovery cost (the attacker must know the path) and enable richer controls (auditd / SELinux / AppArmor on file access, in-place rotation without a restart, no exposure in `kubectl describe pod`) — but they do not stop in-process code from reading the secret.

Combine with the extension-security and operational controls below for meaningful protection.

### Extension security

- Review extension source code before installation, especially C extensions.
- Pin extension versions and require change control for updates.
- Monitor extension installations and updates via the `pg_extension` catalog.
- Be cautious of extensions from unmaintained or single-author projects.
- **Audit grants on sensitive functions:** Check who has execute permission on extensions that access OS resources:
  ```sql
  SELECT grantee, privilege_type FROM information_schema.role_function_grants
  WHERE function_name = 'environment_variables';
  ```

### Operational

- Limit superuser accounts to the minimum needed; audit `pg_roles` regularly.
- On systemd, `EnvironmentFile=` and inline `Environment=` are **equally exposed** — both inject the value into the process environment, where `environment_variables()` can read it. `EnvironmentFile=` only protects the value at rest on disk (via `0600` permissions on the file); it offers no protection at runtime against this extension. To avoid runtime exposure, use [file-mounted secrets](#file-mounted-secrets) above.
- Monitor for unexpected environment variable access patterns.
