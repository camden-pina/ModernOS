BEGIN IMMEDIATE;
INSERT INTO concurrency VALUES(42);
.shell /bin/touch /tmp/linux-ports-work/sqlite-ready
.shell /bin/build-sh /usr/share/coast/linux-ports/wait-release.sh
COMMIT;
