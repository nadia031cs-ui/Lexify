# Lexify API contract

Base URL: `/api` (change in `frontend/js/api.js` -> `API_BASE`, or set `window.LEXIFY_API_BASE` before loading it).
All requests/responses are JSON. Logged-in requests send `Authorization: Bearer <token>`.

Errors: any non-2xx status with `{ "message": "...", "errors": { "field": "message" } }` (`errors` optional).
`401` on a logged-in request = token expired/invalid -> frontend clears the session and opens `login.html`.

| Page | Method & path | Body | Success response |
|---|---|---|---|
| signup.html | `POST /auth/signup` | `{name, email, password}` | `201 {user}` (422 field errors, 409 email taken) |
| login.html | `POST /auth/login` | `{email, password}` | `200 {token, user}` (401 wrong email/password) |
| logout.html | `POST /auth/logout` | - | `200 {ok:true}` |
| account.html | `GET /user/me` | - | `200 {user}` |
| account.html | `PUT /user/profile` | `{name, email, lang}` | `200 {user}` (422 field errors) |
| account.html | `PUT /user/password` | `{currentPassword, newPassword}` | `200 {ok:true}` (400 wrong current password) |
| download.html | `POST /downloads` | `{fileName, language, code}` | `201 {download}` |
| c.html / cpp.html / java.html (Run) | `POST /compile` | `{code}` (Lexify `.mc` source) | `200 {ok, tac, errors, ast, symtab}` |

`/compile` response: on success `{ok:true, tac:"...", ast:"...", symtab:"...", errors:[]}`; on a lexical/syntax/semantic error `{ok:false, errors:["Line 3: ..."]}` (still HTTP 200; use 4xx/5xx only for bad requests or server failure). Guests may call it; if a token is sent the server should increment `runs`.

`user` = `{ id, name, email, lang: "C"|"C++"|"Java", runs, files }` (`files` = number of downloads).

Rules used by the frontend: signup password min 6 chars, change-password min 8 chars (as in the original forms).
Hash passwords on the server (bcrypt / password_hash / argon2); never return the hash.

If the backend is on a different origin than the website (e.g. site on :5500, API on :8000) enable CORS for
`Authorization` and `Content-Type` headers and for `GET, POST, PUT, OPTIONS`.

## Try it without a backend
```
node mock-server/server.js      # then open http://localhost:3000
```
In-memory only (data resets on restart). It implements exactly the table above.
