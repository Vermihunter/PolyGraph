-- Illustrative application schema; mapping validation never executes this file.
CREATE SCHEMA app;
CREATE TABLE app.person (id bigint PRIMARY KEY, name text NOT NULL);
CREATE TABLE app.post (id uuid PRIMARY KEY, content text);
CREATE TABLE app.comment (
    id text PRIMARY KEY,
    content text,
    reply_to_post_id uuid REFERENCES app.post(id)
);
CREATE TABLE app.person_knows_person (
    person_id bigint NOT NULL REFERENCES app.person(id),
    friend_id bigint NOT NULL REFERENCES app.person(id),
    since date
);
