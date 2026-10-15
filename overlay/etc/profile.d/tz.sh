# With TZ unset, glibc stats /etc/localtime and re-checks the zone on every
# localtime() call (once per file in "ls -l" or an mc panel).  Naming the
# file loads it once per process.
[ -n "$TZ" ] || export TZ=:/etc/localtime
