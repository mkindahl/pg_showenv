\echo Use "CREATE EXTENSION showenv" to load this file. \quit

-- showenv: expose server process environment variables.
--
-- Restricted to superusers to limit the blast radius — this function can
-- reveal POSTGRES_PASSWORD, DATABASE_URL, and any other secret injected as an env var.

CREATE FUNCTION environment_variables()
RETURNS TABLE(name text, value text)
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Only superusers should be able to see environment variables, so we don't
-- grant this function to PUBLIC.
REVOKE ALL ON FUNCTION environment_variables() FROM PUBLIC;
