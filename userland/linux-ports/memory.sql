SELECT json_extract('{"n":42}', '$.n');
CREATE VIRTUAL TABLE docs USING fts5(body);
INSERT INTO docs VALUES('modern operating system');
SELECT count(*) FROM docs WHERE docs MATCH 'modern';
