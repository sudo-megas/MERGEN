set -u
MERGEN=<repo>/build/mergen
export QT_QPA_PLATFORM=offscreen
SOCK="${XDG_RUNTIME_DIR:-/tmp}/mergen-$(id -u).sock"
fails=0
chk() { if [ "$1" = "1" ]; then printf "  PASS  %s\n" "$2"; else printf "  FAIL  %s\n" "$2"; fails=$((fails+1)); fi; }

say() { python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2)
try:
    s.connect('$SOCK')
except Exception as e:
    print('NOSOCKET'); sys.exit(0)
s.sendall((sys.argv[1]+'\n').encode())
print(s.recv(4096).decode().strip())
" "$1"; }

rm -f "$SOCK"
echo; echo "--- no instance running ---"
r=$(say "goto 2"); chk "$([ "$r" = "NOSOCKET" ] && echo 1 || echo 0)" "nothing answers before launch (got: $r)"

echo; echo "--- start one instance ---"
$MERGEN outline.pdf & M1=$!
sleep 2
[ -S "$SOCK" ] && chk 1 "the socket exists at \$XDG_RUNTIME_DIR/mergen-\$UID.sock" || chk 0 "socket missing"

echo; echo "--- commands, driven from plain python (no Qt, no session bus) ---"
r=$(say "goto 3");         chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "goto 3        -> $r"
r=$(say "goto 99");        chk "$([ "${r#err:}" != "$r" ] && echo 1 || echo 0)" "goto 99       -> $r"
r=$(say "goto banana");    chk "$([ "${r#err:}" != "$r" ] && echo 1 || echo 0)" "goto banana   -> $r"
r=$(say "search needle");  chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "search needle -> $r"
sleep 1
r=$(say "next");           chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "next          -> $r"
r=$(say "prev");           chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "prev          -> $r"
r=$(say "open $PWD/test.pdf"); chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "open <path>   -> $r"
r=$(say "open /nope.pdf"); chk "$([ "${r#err:}" != "$r" ] && echo 1 || echo 0)" "open missing  -> $r"
r=$(say "frobnicate");     chk "$([ "${r#err:}" != "$r" ] && echo 1 || echo 0)" "unknown verb  -> $r"

echo; echo "--- a second launch hands over and leaves ---"
before=$(pgrep -fc "build/mergen" || echo 0)
$MERGEN "$PWD/annot.pdf"; rc=$?
sleep 1
after=$(pgrep -fc "build/mergen" || echo 0)
chk "$([ "$rc" = "0" ] && echo 1 || echo 0)" "the second process exited cleanly (rc=$rc)"
chk "$([ "$before" = "$after" ] && echo 1 || echo 0)" "still exactly one process ($before -> $after), not two windows"

echo; echo "--- quit over the socket ---"
r=$(say "quit"); chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "quit          -> $r"
sleep 2
kill -0 $M1 2>/dev/null && { chk 0 "the instance exited"; kill -9 $M1 2>/dev/null; } || chk 1 "the instance exited"

echo; echo "--- a stale socket from a crashed instance is taken over ---"
rm -f "$SOCK"
python3 -c "
import socket
s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind('$SOCK'); s.close()
"   # a file that looks like a socket but nothing is listening
[ -S "$SOCK" ] && echo "      left a stale socket file behind"
$MERGEN outline.pdf & M2=$!
sleep 2
r=$(say "goto 2")
chk "$([ "$r" = "ok" ] && echo 1 || echo 0)" "a fresh instance replaced the stale socket (got: $r)"
kill $M2 2>/dev/null; sleep 1; kill -9 $M2 2>/dev/null
rm -f "$SOCK"

echo
[ "$fails" = "0" ] && echo "ALL CHECKS PASSED" || echo "$fails CHECK(S) FAILED"
exit $fails
