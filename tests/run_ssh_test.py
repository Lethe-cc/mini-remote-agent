import getpass
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def main():
    sshd = shutil.which("sshd")
    keygen = shutil.which("ssh-keygen")
    if not sshd or not keygen:
        raise SystemExit("OpenSSH server and ssh-keygen are required")
    with tempfile.TemporaryDirectory(prefix="mrc-test-") as temporary:
        root = pathlib.Path(temporary)
        os.mkfifo(root / "source.fifo")
        for name in ("host", "client", "wrong"):
            subprocess.run([keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(root / name)], check=True)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = root / "sshd_config"
        config.write_text(
            f"Port {port}\nListenAddress 127.0.0.1\nHostKey {root / 'host'}\n"
            f"PidFile {root / 'sshd.pid'}\nAuthorizedKeysFile {root / 'client.pub'}\n"
            "StrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n"
            "UsePAM no\nPermitRootLogin prohibit-password\nSubsystem sftp internal-sftp\n"
        )
        wrong_key = (root / "wrong.pub").read_text().split()
        (root / "wrong_known_hosts").write_text(f"[127.0.0.1]:{port} {wrong_key[0]} {wrong_key[1]}\n")
        with (root / "server.log").open("w+") as log:
            server = subprocess.Popen([sshd, "-D", "-e", "-f", str(config)], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    if server.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(log.read())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise RuntimeError("Test SSH server startup timed out")
                        time.sleep(0.05)
                subprocess.run([sys.argv[1], str(port), getpass.getuser(), str(root / "client"),
                                str(root), str(root / "wrong_known_hosts")], check=True, timeout=90)
            except Exception:
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == "__main__":
    main()