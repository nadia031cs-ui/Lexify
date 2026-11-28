# Lexify - build & deploy

## 1. Build on Windows (every laptop, after every `git pull`)

Needs: **gcc** (MinGW-w64) and **Bison 3.x + Flex** (install *winflexbison*: it gives `win_bison` / `win_flex`;
add its folder to PATH). Old GnuWin32 Bison 2.4 cannot build `parser.y`.

```
win_bison --version      (must say 3.x)
build.bat                (run inside the Lexify folder)
```

`build.bat` regenerates `build/parser.tab.c` and `build/lex.yy.c` from `src/` every time, then builds `lexify.exe`.
Never copy or commit `build/` or `lexify.exe`: each laptop builds its own.
`prototypes/backend/.env` must not contain a `LEXIFY_BIN=` path of one laptop; the server finds `lexify.exe` in the
Lexify folder by itself.

Run locally:
```
cd prototypes/backend
npm install
npm start                 (http://localhost:3000)
```

## 2. Deploy on Render (Docker, free)

The `Dockerfile` installs gcc, flex and bison on Linux, builds the compiler with `make`, runs `make test`,
then starts `prototypes/backend` (it also serves the website from `prototypes/frontend`).

1. Push the repo (with `Dockerfile` and `.dockerignore`) to GitHub.
2. render.com -> sign in with GitHub -> **+ New** -> **Web Service**.
3. Git Provider -> give Render access to this repo (Only select repositories) -> **Connect**.
4. Name: `lexify` - Language: **Docker** - Branch: `main` - Region: **Singapore** - Root Directory: empty -
   Instance type: **Free** - no environment variables - Health check path: `/api/health`.
5. **Deploy Web Service**, wait until the status is **Live** (5-10 min), open `https://<name>.onrender.com`.

Notes: the free service sleeps after 15 minutes without visitors (first request then takes about a minute);
the SQLite database (accounts) is reset on every restart / deploy; every `git push` redeploys automatically.
