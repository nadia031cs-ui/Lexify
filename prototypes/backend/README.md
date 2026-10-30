# Lexify Backend

Node.js + Express + SQLite. Implements the contract in the project's root `API.md`
(signup, login, logout, profile, password, downloads), so the existing `js/api.js` works without changes.

## Setup

Put this folder in the repo as `backend/` (next to `prototypes/`), then:

```bash
cd backend
npm install
cp .env.example .env      # Windows: copy .env.example .env
npm start                 # http://localhost:3000
npm test                  # 27 checks: signup -> login -> profile -> password -> downloads -> logout
```

Needs Node 18+. The database file `data/lexify.db` is created automatically.

`.env.example` sets `FRONTEND_DIR=../prototypes/frontend`, so the same server also serves the website:
open **http://localhost:3000** (login.html, account.html, ...). The frontend uses `/api` on the same origin,
so no CORS setup is needed. If you serve the website elsewhere (e.g. VS Code Live Server on :5500),
set `window.LEXIFY_API_BASE = "http://localhost:3000/api"` before loading `js/api.js`
and make sure that origin is in `CORS_ORIGINS`.

## API

JSON in/out. Logged-in requests send `Authorization: Bearer <token>`.
Errors: `{ "message": "...", "errors": { "field": "message" } }` (`errors` optional).

| Method & path | Auth | Body | Success | Errors |
|---|---|---|---|---|
| `POST /api/auth/signup` | no | `name, email, password` | 201 `{user}` | 422 field errors, 409 email taken |
| `POST /api/auth/login` | no | `email, password` | 200 `{token, user}` | 401 wrong email/password |
| `POST /api/auth/logout` | yes | - | 200 `{ok:true}` | 401 |
| `POST /api/auth/logout-all` | yes | - | 200 `{ok:true}` | 401 |
| `GET /api/user/me` | yes | - | 200 `{user}` | 401 |
| `PUT /api/user/profile` | yes | `name, email, lang` | 200 `{user}` | 422 field errors |
| `PUT /api/user/password` | yes | `currentPassword, newPassword` | 200 `{ok:true}` | 400 wrong current, 422 weak/same |
| `GET /api/downloads` | yes | - | 200 `{downloads:[{id,fileName,language,createdAt}]}` | 401 |
| `POST /api/downloads` | yes | `fileName, language, code` | 201 `{download}` | 422 |
| `GET /api/downloads/:id` | yes | - | 200 `{download}` incl. `code` | 404 |
| `DELETE /api/downloads/:id` | yes | - | 200 `{ok:true}` | 404 |
| `GET /api/health` | no | - | 200 `{ok:true}` | |

`user` = `{ id, name, email, lang: "C"|"C++"|"Java", runs, files }` (`files` = number of downloads, `runs` is 0 until the compile API counts runs).
`language` for downloads: `"C"`, `"C++"` or `"Java"`.

## Rules

- Signup password: min 6 chars. Change password: min 8 chars, must differ from the current one (same as the forms).
  Max 72 bytes (bcrypt limit).
- Passwords are hashed with bcrypt, never returned. Emails are stored lowercase and are unique.
- Session token = 32 random bytes; only its SHA-256 hash is stored. Expires after `SESSION_TTL_HOURS` (default 24).
  Expired/invalid token -> 401, which makes `api.js` clear the session and open `login.html`.
- Changing the password logs out all other devices.
- Rate limit on `/api/auth/*`: 50 requests / 15 min per IP.
- JSON body limit 1 MB, code in a download max 500,000 characters.

## Database (SQLite)

- `users(id, name, email, password_hash, lang, runs, created_at, updated_at)`
- `sessions(id, user_id, token_hash, created_at, expires_at)`
- `downloads(id, user_id, file_name, language, code, created_at)`

If you change a table, delete `data/lexify.db` (dev data only) so it is recreated.

## Not done yet

- `POST /api/compile` (compiler integration) and counting `runs`.
- HTTPS / deployment setup. For a live deploy, put it behind HTTPS and set `CORS_ORIGINS` to your real site.
