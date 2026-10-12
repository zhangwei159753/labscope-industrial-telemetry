#!/usr/bin/env python3
"""Create the first web administrator without putting the password in argv/history."""

import argparse
import getpass
import re
import subprocess
import sys
from pathlib import Path


USERNAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.@-]{2,63}$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("username")
    parser.add_argument("--compose-file", default="compose.yaml")
    args = parser.parse_args()
    if not USERNAME.fullmatch(args.username):
        parser.error("username must be 3-64 letters, digits, dot, underscore, @, or hyphen")

    password = getpass.getpass("New administrator password (12-1024 characters): ")
    confirmation = getpass.getpass("Confirm password: ")
    if password != confirmation or not 12 <= len(password.encode("utf-8")) <= 1024:
        print("Password confirmation failed or password length is invalid.", file=sys.stderr)
        return 2

    compose_file = Path(args.compose_file)
    command = [
        "docker", "compose", "-f", str(compose_file), "exec", "-T", "app",
        "/usr/local/bin/lab_telemetry", "--create-admin", args.username,
    ]
    try:
        result = subprocess.run(
            command,
            input=f"{password}\n{confirmation}\n",
            text=True,
            check=False,
        )
    except OSError as error:
        print(f"Could not run Docker Compose: {error}", file=sys.stderr)
        return 2
    finally:
        password = ""
        confirmation = ""
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
