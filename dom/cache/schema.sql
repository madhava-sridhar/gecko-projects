CREATE TABLE map (
  id INTEGER NOT NULL PRIMARY KEY,
  request_method TEXT NOT NULL,
  request_url TEXT NOT NULL,
  request_url_no_query TEXT NOT NULL,
  request_mode INTEGER NOT NULL,
  request_credentials INTEGER NOT NULL,
  response_type INTEGER NOT NULL,
  response_status INTEGER NOT NULL,
  response_status_text TEXT NOT NULL
);
CREATE TABLE request_headers (
  name TEXT NOT NULL,
  value TEXT NOT NULL,
  map_id INTEGER NOT NULL REFERENCES map(id)
);
CREATE TABLE response_headers (
  name TEXT NOT NULL,
  value TEXT NOT NULL,
  map_id INTEGER NOT NULL REFERENCES map(id)
);

INSERT INTO map (
  id,
  request_method,
  request_url,
  request_url_no_query,
  request_mode,
  request_credentials,
  response_type,
  response_status,
  response_status_text
) VALUES (
  1,
  'GET',
  'http://example.com?q=foobar',
  'http://example.com',
  'cors',
  'same-origin',
  'cors',
  '200',
  'OK'
);

INSERT INTO map (
  id,
  request_method,
  request_url,
  request_url_no_query,
  request_mode,
  request_credentials,
  response_type,
  response_status,
  response_status_text
) VALUES (
  2,
  'GET',
  'http://example.com?q=snafu',
  'http://example.com',
  'cors',
  'same-origin',
  'cors',
  '200',
  'OK'
);

INSERT INTO map (
  id,
  request_method,
  request_url,
  request_url_no_query,
  request_mode,
  request_credentials,
  response_type,
  response_status,
  response_status_text
) VALUES (
  3,
  'GET',
  'http://example.com/vary-test?q=snafu',
  'http://example.com/vary-test',
  'cors',
  'same-origin',
  'cors',
  '200',
  'OK'
);

INSERT INTO response_headers (
  name,
  value,
  map_id
) VALUES (
  'vary',
  'host',
  3
);

SELECT id, COUNT(response_headers.name) AS vary_count
FROM map
LEFT OUTER JOIN response_headers ON map.id=response_headers.map_id
                                AND response_headers.name='vary'
WHERE map.request_url LIKE 'http%'
GROUP BY map.id;
