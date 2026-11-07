/* Lexify mock backend: for testing the frontend WITHOUT a real backend.
   run:  node mock-server/server.js   then open http://localhost:3000
   Data is kept in memory only (resets when you stop the server).
   Replace with your real backend using the same API (see API.md). */
const http = require("http");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const os = require("os");
const { execFile } = require("child_process");

const PORT = process.env.PORT || 3000;
const WEB_ROOT = [path.join(__dirname, "..", "prototypes", "frontend"), path.join(__dirname, "..", "frontend")].find(d => fs.existsSync(d)) || path.join(__dirname, "..", "frontend");
const BIN = process.env.LEXIFY_BIN || path.join(__dirname, "..", "lexify");
const MIME = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css", ".png": "image/png", ".json": "application/json" };

const users = new Map();     // email -> { id, name, email, lang, salt, hash, downloads: [] }
const sessions = new Map();  // token -> user object
let nextId = 1;

const hashPw = (pw, salt) => crypto.scryptSync(pw, salt, 32).toString("hex");
const isEmail = e => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(e);
const publicUser = u => ({ id: u.id, name: u.name, email: u.email, lang: u.lang, runs: u.runs || 0, files: u.downloads.length });

function send(res, status, data) {
    res.writeHead(status, { "Content-Type": "application/json" });
    res.end(JSON.stringify(data));
}
const fail = (res, status, message, errors) => send(res, status, errors ? { message, errors } : { message });

function readBody(req) {
    return new Promise(resolve => {
        let raw = "";
        req.on("data", c => { raw += c; if (raw.length > 1e6) req.destroy(); });
        req.on("end", () => { try { resolve(raw ? JSON.parse(raw) : {}); } catch (e) { resolve(null); } });
    });
}

function authUser(req) {
    const m = (req.headers.authorization || "").match(/^Bearer (.+)$/);
    const user = m && sessions.get(m[1]);
    return user ? { user, token: m[1] } : null;
}


function runBin(file, flag) {
    return new Promise(resolve => execFile(BIN, flag ? [file, flag] : [file], { timeout: 5000, maxBuffer: 1 << 20 },
        (err, stdout, stderr) => resolve({ err, stdout, stderr })));
}
const stripTac = (out, tac) => (tac && out.endsWith(tac) ? out.slice(0, out.length - tac.length) : out);

async function compile(code) {
    const file = path.join(os.tmpdir(), "lexify-" + crypto.randomUUID() + ".mc");
    fs.writeFileSync(file, code);
    try {
        const main = await runBin(file);
        if (main.err && main.err.code === "ENOENT") return { status: 500, data: { message: "Compiler binary not found. Run `make` or set LEXIFY_BIN." } };
        if (main.err && main.err.killed) return { status: 500, data: { message: "Compiler timed out." } };
        if (main.err) {
            const errors = main.stderr.split("\n").map(x => x.trim()).filter(Boolean);
            return { status: 200, data: { ok: false, errors: errors.length ? errors : ["Compilation failed."] } };
        }
        const [a, st] = await Promise.all([runBin(file, "--ast"), runBin(file, "--symtab")]);
        return { status: 200, data: { ok: true, tac: main.stdout, ast: stripTac(a.stdout, main.stdout), symtab: stripTac(st.stdout, main.stdout), errors: [] } };
    } finally { try { fs.unlinkSync(file); } catch (e) { } }
}

async function api(req, res, route) {
    const body = ["POST", "PUT"].includes(req.method) ? await readBody(req) : {};
    if (body === null) return fail(res, 400, "Invalid JSON.");

    if (route === "POST /auth/signup") {
        const name = String(body.name || "").trim(), email = String(body.email || "").trim().toLowerCase(), pw = String(body.password || "");
        const errors = {};
        if (!name) errors.name = "Please enter your name.";
        if (!isEmail(email)) errors.email = "Please enter a valid email.";
        if (pw.length < 6) errors.password = "Password must be at least 6 characters.";
        if (Object.keys(errors).length) return fail(res, 422, "Please fix the errors.", errors);
        if (users.has(email)) return fail(res, 409, "This email is already registered.", { email: "This email is already registered." });
        const salt = crypto.randomBytes(16).toString("hex");
        const u = { id: nextId++, name, email, lang: "C", salt, hash: hashPw(pw, salt), downloads: [] };
        users.set(email, u);
        return send(res, 201, { user: publicUser(u) });
    }

    if (route === "POST /auth/login") {
        const u = users.get(String(body.email || "").trim().toLowerCase());
        const ok = u && crypto.timingSafeEqual(Buffer.from(u.hash), Buffer.from(hashPw(String(body.password || ""), u.salt)));
        if (!ok) return fail(res, 401, "Incorrect email or password.");
        const token = crypto.randomBytes(32).toString("hex");
        sessions.set(token, u);
        return send(res, 200, { token, user: publicUser(u) });
    }

    // Guests may compile; a valid token also counts the run
    if (route === "POST /compile") {
        const code = body && body.code;
        if (typeof code !== "string" || !code.trim()) return fail(res, 400, "Source code is required.");
        if (code.length > 100000) return fail(res, 413, "Source code too large.");
        const r = await compile(code);
        if (r.status === 200) { const a = authUser(req); if (a) a.user.runs = (a.user.runs || 0) + 1; }
        return send(res, r.status, r.data);
    }

    // Everything below needs a valid token
    const auth = authUser(req);
    if (!auth) return fail(res, 401, "Please log in.");
    const u = auth.user;

    if (route === "POST /auth/logout") { sessions.delete(auth.token); return send(res, 200, { ok: true }); }
    if (route === "GET /user/me") return send(res, 200, { user: publicUser(u) });

    if (route === "PUT /user/profile") {
        const name = String(body.name || "").trim(), email = String(body.email || "").trim().toLowerCase(), lang = String(body.lang || "C");
        const errors = {};
        if (!name) errors.name = "Please enter your name.";
        if (!isEmail(email)) errors.email = "Please enter a valid email address.";
        if (!["C", "C++", "Java"].includes(lang)) errors.lang = "Unsupported language.";
        if (email !== u.email && users.has(email)) errors.email = "This email is already used by another account.";
        if (Object.keys(errors).length) return fail(res, 422, "Please fix the errors.", errors);
        if (email !== u.email) {
            users.delete(u.email); u.email = email; users.set(email, u);
        }
        u.name = name; u.lang = lang;
        return send(res, 200, { user: publicUser(u) });
    }

    if (route === "PUT /user/password") {
        const cur = String(body.currentPassword || ""), next = String(body.newPassword || "");
        if (hashPw(cur, u.salt) !== u.hash) return fail(res, 400, "Current password is incorrect.");
        if (next.length < 8) return fail(res, 422, "New password must be at least 8 characters.");
        if (next === cur) return fail(res, 422, "New password must be different.");
        u.salt = crypto.randomBytes(16).toString("hex"); u.hash = hashPw(next, u.salt);
        return send(res, 200, { ok: true });
    }

    if (route === "POST /downloads") {
        const fileName = String(body.fileName || "").slice(0, 200), language = String(body.language || "");
        if (!fileName || !["C", "C++", "Java"].includes(language)) return fail(res, 422, "fileName and language are required.");
        const rec = { id: u.downloads.length + 1, fileName, language, size: String(body.code || "").length, at: new Date().toISOString() };
        u.downloads.push(rec);
        return send(res, 201, { download: rec });
    }

    return fail(res, 404, "Not found.");
}

http.createServer((req, res) => {
    const url = new URL(req.url, "http://localhost");
    if (url.pathname.startsWith("/api/")) {
        return api(req, res, req.method + " " + url.pathname.slice(4)).catch(e => { console.error(e); fail(res, 500, "Server error."); });
    }
    let file = path.normalize(path.join(WEB_ROOT, url.pathname === "/" ? "index.html" : decodeURIComponent(url.pathname)));
    if (!file.startsWith(WEB_ROOT)) { res.writeHead(403); return res.end(); }
    fs.readFile(file, (err, data) => {
        if (err) { res.writeHead(404); return res.end("Not found"); }
        res.writeHead(200, { "Content-Type": MIME[path.extname(file)] || "application/octet-stream" });
        res.end(data);
    });
}).listen(PORT, () => console.log("Lexify mock server: http://localhost:" + PORT));
