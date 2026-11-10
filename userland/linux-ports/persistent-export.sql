.output /mnt/nvme0n1p1/payload.txt
SELECT content FROM persisted WHERE n=1;
.output stdout
SELECT length(content) FROM persisted WHERE n=1;
PRAGMA user_version;
PRAGMA integrity_check;
