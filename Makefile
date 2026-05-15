EXTENSION = showenv
MODULE_big = showenv
OBJS       = showenv.o

DATA = showenv--1.0.sql

PG_CONFIG  ?= pg_config
PGXS       := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
