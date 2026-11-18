# core dumps into the boot dir are size-limited (512-byte blocks: ~4 MB)
ulimit -c 8192 2>/dev/null
export PATH=$PATH:/usr/local/sbin
