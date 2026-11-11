-- Application-owned normalized tables. No PolyGraph columns or traversal indexes.
CREATE SCHEMA app;
CREATE SCHEMA geo;
CREATE SCHEMA refs;
CREATE TABLE app.person (
    id bigint PRIMARY KEY,
    name text,
    active boolean NOT NULL,
    total bigint,
    manager bigint REFERENCES app.person(id)
);
CREATE TABLE geo.place (code text PRIMARY KEY, label text);
CREATE TABLE app.document (
    id uuid PRIMARY KEY,
    title text,
    place text REFERENCES geo.place(code)
);
-- Compatible, application-owned reference rows for remote graph vertices.
CREATE TABLE refs.gateway (id bigint PRIMARY KEY, name text);
CREATE TABLE refs.sensor (code text PRIMARY KEY, name text);
CREATE TABLE app.reads (
    id text PRIMARY KEY,
    person bigint NOT NULL REFERENCES app.person(id),
    document uuid NOT NULL REFERENCES app.document(id),
    weight double precision
);
CREATE TABLE app.opens (
    place text REFERENCES geo.place(code),
    gateway bigint REFERENCES refs.gateway(id)
);
CREATE TABLE refs.next (gateway bigint REFERENCES refs.gateway(id), sensor text REFERENCES refs.sensor(code));
CREATE TABLE refs.returns (sensor text REFERENCES refs.sensor(code), person bigint REFERENCES app.person(id));
CREATE TABLE app.sentinel (id integer PRIMARY KEY, payload text NOT NULL);
INSERT INTO app.sentinel VALUES (99, 'untouched, 雪');
