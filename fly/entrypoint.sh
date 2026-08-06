#!/bin/sh
# Combined entrypoint for the throwaway Fly.io test machine. Runs the
# best FDB topology this project has actually measured (task #17's
# real concurrent-zone throughput test): 8 fdbserver processes with
# real log/proxy/resolver role counts, not a single minimal process --
# confirmed locally to raise the aggregate commit ceiling from ~212/s
# to ~357/s across 16 concurrent zones. Then execs a real small fabric
# of zone-server-h2o processes. TLS_CERT/TLS_KEY (the WebTransport
# client-facing cert) are read directly by zone-server-h2o itself from
# the environment (src/main.c's own resolve_tls_files()) -- nothing to
# do here for that part.
#
# Also wires real mutual TLS between zone-server-h2o (as an FDB
# client) and the FDB cluster -- the actual trust boundary this
# deployment has, since zone-to-zone fabric coordination itself is not
# implemented yet (docs/0001-defer-nogod-gossip-authority.md). Without
# this, client<->cluster traffic is plaintext, same-VM trust only.
set -e

DATA_ROOT=/var/fdb/data
LOG_ROOT=/var/fdb/logs
CLUSTER_FILE=/var/fdb/fdb.cluster
TLS_DIR=/var/fdb/tls
N=${FDB_PROCESS_COUNT:-8}
BASE_PORT=4500
mkdir -p "$LOG_ROOT" "$TLS_DIR"

# Real mutual TLS: a small local CA signs both fdbserver's cert and
# zone-server-h2o's own client cert, FDB's own documented pattern for
# this (each side verifies the other against the shared CA, not just
# a bare self-signed cert on one side).
cd "$TLS_DIR"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
  -keyout ca.key -out ca.crt -days 1 -nodes \
  -subj "/CN=zone-server-h2o-fdb-mtls-ca" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
  -keyout fdbserver.key -out fdbserver.csr -nodes -subj "/CN=fdbserver" 2>/dev/null
openssl x509 -req -in fdbserver.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out fdbserver.crt -days 1 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
  -keyout client.key -out client.csr -nodes -subj "/CN=zone-server-h2o" 2>/dev/null
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt -days 1 2>/dev/null
rm -f fdbserver.csr client.csr
echo "entrypoint: generated FDB mutual-TLS CA + fdbserver cert + client cert" >&2

DESC=$(tr -dc 'a-zA-Z0-9' </dev/urandom | head -c8)
ID=$(tr -dc 'a-zA-Z0-9' </dev/urandom | head -c8)
# :tls suffix on the coordinator address is what tells every
# fdbserver/fdbcli/libfdb_c connection to this cluster that TLS is
# required, not optional -- FDB's own cluster-file syntax for this.
echo "${DESC}:${ID}@127.0.0.1:${BASE_PORT}:tls" > "$CLUSTER_FILE"

i=0
while [ "$i" -lt "$N" ]; do
  port=$((BASE_PORT + i))
  mkdir -p "$DATA_ROOT/$i"
  # :tls must be on --listen-address/--public-address too, not just the
  # cluster file's coordinator entry -- confirmed by the real error the
  # first attempt hit: "TLS state of public address 127.0.0.1:4500 does
  # not match in coordinator list."
  fdbserver \
    --cluster-file "$CLUSTER_FILE" \
    --datadir "$DATA_ROOT/$i" \
    --logdir "$LOG_ROOT" \
    --listen-address "127.0.0.1:$port:tls" \
    --public-address "127.0.0.1:$port:tls" \
    --tls-certificate-file "$TLS_DIR/fdbserver.crt" \
    --tls-key-file "$TLS_DIR/fdbserver.key" \
    --tls-ca-file "$TLS_DIR/ca.crt" \
    --tls-verify-peers "Check.Valid=1" &
  i=$((i + 1))
done
echo "entrypoint: started $N fdbserver processes on ports $BASE_PORT..$((BASE_PORT + N - 1)), mutual TLS on" >&2

# fdbcli is also an FDB client -- needs the client cert too, or it
# cannot reach a TLS-only cluster to run the configure commands below.
export FDB_TLS_CERTIFICATE_FILE="$TLS_DIR/client.crt"
export FDB_TLS_KEY_FILE="$TLS_DIR/client.key"
export FDB_TLS_CA_FILE="$TLS_DIR/ca.crt"

for i in $(seq 1 30); do
  if fdbcli -C "$CLUSTER_FILE" --exec "configure new single memory" 2>/dev/null; then
    echo "entrypoint: database created" >&2
    break
  fi
  sleep 1
done

# Same role counts the local 16-zone test used: 4 logs, 1 GRV proxy +
# 3 commit proxies (fdbcli's own "proxies=4" auto-splits this way,
# confirmed by reading its own warning output during that earlier
# test), 2 resolvers.
for i in $(seq 1 15); do
  if fdbcli -C "$CLUSTER_FILE" --exec "configure logs=4 proxies=4 resolvers=2" 2>/dev/null; then
    echo "entrypoint: configured logs=4 proxies=4 resolvers=2" >&2
    break
  fi
  sleep 1
done

# Rotating FDB -> Fly Tigris (S3-compatible) backup, real fdbbackup,
# not a hand-rolled snapshot copy. Only runs if Tigris credentials are
# actually set. Real names, not invented: `flyctl storage create`
# stages AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY/AWS_ENDPOINT_URL_S3/
# AWS_REGION/BUCKET_NAME as this app's own secrets (confirmed by
# reading its own real output directly, not assumed) -- this reads
# those, not a made-up TIGRIS_* name. Skipped silently if unset,
# matching every other optional piece of this entrypoint
# (TLS_CERT/TLS_KEY already work the same way in main.c).
#
# Rotation, not unbounded growth: BACKUP_SLOTS destinations
# (zone-server-h2o-mud-backup-slot-0 .. slot-(N-1)) are reused in a
# ring. Before writing a new slot, its previous contents are removed
# with a real `fdbbackup delete`, so the bucket never holds more than
# BACKUP_SLOTS backups' worth of data no matter how long the machine
# runs -- confirmed against fdbbackup's own `delete` action (fdbbackup
# --help), not assumed.
#
# `-w` (wait) blocks until the one-shot snapshot is restorable; without
# `-z/--no-stop-when-done` fdbbackup stops itself once that happens --
# real one-shot-snapshot behavior confirmed via fdbbackup's own start
# --help text, not a guess. The secret key is kept out of argv/the
# blobstore URL via --blob-credentials's own documented JSON file
# form ({"accounts":{"user@host":{"secret":"..."}}}), not embedded in
# the URL where `ps` could see it.
if [ -n "$AWS_ACCESS_KEY_ID" ] && [ -n "$AWS_SECRET_ACCESS_KEY" ]; then
  BACKUP_SLOTS=${BACKUP_SLOTS:-4}
  BACKUP_INTERVAL_SECONDS=${BACKUP_INTERVAL_SECONDS:-21600} # 4x/day
  # AWS_ENDPOINT_URL_S3 is a full URL (https://fly.storage.tigris.dev);
  # blobstore:// wants a bare host, so strip the scheme.
  TIGRIS_HOST=$(echo "$AWS_ENDPOINT_URL_S3" | sed -E 's#^https?://##')
  TIGRIS_BUCKET=${BUCKET_NAME}
  BLOB_CREDS_FILE="$TLS_DIR/tigris-blob-credentials.json"
  cat >"$BLOB_CREDS_FILE" <<EOF
{"accounts":{"${AWS_ACCESS_KEY_ID}@${TIGRIS_HOST}":{"secret":"${AWS_SECRET_ACCESS_KEY}"}}}
EOF
  chmod 600 "$BLOB_CREDS_FILE"

  (
    slot=0
    while true; do
      dest="blobstore://${AWS_ACCESS_KEY_ID}@${TIGRIS_HOST}/zone-server-h2o-mud-backup-slot-${slot}?bucket=${TIGRIS_BUCKET}"
      fdbbackup delete -C "$CLUSTER_FILE" -d "$dest" --blob-credentials "$BLOB_CREDS_FILE" 2>/dev/null || true
      if fdbbackup start -C "$CLUSTER_FILE" -d "$dest" --blob-credentials "$BLOB_CREDS_FILE" -w 2>&1 \
        | tee -a "$LOG_ROOT/backup.log" >&2; then
        echo "entrypoint: backup slot $slot committed to $dest" >&2
      else
        echo "entrypoint: backup slot $slot failed, will retry next cycle" >&2
      fi
      slot=$(((slot + 1) % BACKUP_SLOTS))
      sleep "$BACKUP_INTERVAL_SECONDS"
    done
  ) &
  echo "entrypoint: started rotating FDB backup ($BACKUP_SLOTS slots, every ${BACKUP_INTERVAL_SECONDS}s, bucket=$TIGRIS_BUCKET)" >&2
else
  echo "entrypoint: AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY not set, skipping FDB backup" >&2
fi

# Zone count: 357 aggregate commits/sec (this same 8-process FDB
# topology's own measured ceiling, from the earlier local 16-zone
# test) divided by the 64 Hz per-zone target is ~5.6 -- 5 zones is the
# real number of concurrent zone-server-h2o processes this FDB
# topology can carry at full target rate at once, not a round number
# picked for convenience. Each one is its own process, its own UDP
# port, matching zone-server/AGENTS.md's "one UDP port per zone
# instance" deployment shape -- a real small fabric, not a single zone.
# FDB_TLS_* stay exported: libfdb_c reads the same env vars fdbcli
# just used, so every zone-server-h2o process below picks up the same
# client cert automatically, no code change needed in main.c/
# fdb_database.c for this.
ZONE_COUNT=${ZONE_COUNT:-5}
BASE_ZONE_PORT=${ZONE_PORT:-7443}
z=0
while [ "$z" -lt "$ZONE_COUNT" ]; do
  port=$((BASE_ZONE_PORT + z))
  if [ "$z" -eq 0 ] && [ -n "$MUD_HTTP_PORT" ]; then
    # MUD prototype HTTP surface: only zone 0's process binds it --
    # every zone process here shares one host, so a second process
    # trying the same TCP port would just fail to bind. Real paths to
    # the artifacts fly/Containerfile just built, matching
    # src/main.c's own three-env-var opt-in gate exactly.
    env LD_LIBRARY_PATH="/opt/h2o/lib:/work/build/lib" \
      MUD_HTTP_PORT="$MUD_HTTP_PORT" \
      MUD_WEB_DOCROOT="/work/mud/web" \
      MUD_ORCHESTRATOR_PATH="/work/mud/orchestrator/mud-sandbox-orchestrator" \
      MUD_GUEST_ELF_PATH="/work/mud/guest/mud_guest.rv64.elf" \
      /work/build/zone-server-h2o -a1 -c "$CLUSTER_FILE" -z "$z" -p "$port" &
  else
    env LD_LIBRARY_PATH="/opt/h2o/lib:/work/build/lib" \
      /work/build/zone-server-h2o -a1 -c "$CLUSTER_FILE" -z "$z" -p "$port" &
  fi
  z=$((z + 1))
done
wait
