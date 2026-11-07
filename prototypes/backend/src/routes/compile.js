// POST /api/compile  { code }  ->  { ok, tac, ast, symtab, errors }
// Runs the Lexify C compiler (./lexify) on the submitted .mc source.
// Guests may call it; if a valid Bearer token is sent, the user's `runs` counter is incremented.
const express = require('express');
const { execFile } = require('child_process');
const fs = require('fs/promises');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const db = require('../db');
const { hashToken, HttpError } = require('../utils');

const router = express.Router();

// Path to the compiled binary. Override with LEXIFY_BIN in .env.
// Default: Lexify/lexify  (this file is Lexify/prototypes/backend/src/routes/compile.js)
const BIN = process.env.LEXIFY_BIN || path.resolve(__dirname, '../../../../lexify');

const findSession = db.prepare('SELECT user_id, expires_at FROM sessions WHERE token_hash = ?');
const bumpRuns = db.prepare('UPDATE users SET runs = runs + 1 WHERE id = ?');

// Optional auth: never rejects, just counts the run when the token is valid.
function countRun(req) {
  const m = (req.headers.authorization || '').match(/^Bearer\s+([a-f0-9]{64})$/i);
  if (!m) return;
  const s = findSession.get(hashToken(m[1]));
  if (s && s.expires_at >= Date.now()) bumpRuns.run(s.user_id);
}

function run(file, flag) {
  return new Promise(resolve => {
    const args = flag ? [file, flag] : [file];
    execFile(BIN, args, { timeout: 5000, maxBuffer: 1 << 20 }, (err, stdout, stderr) =>
      resolve({ err, stdout, stderr }));
  });
}

// --ast / --symtab print their text first and the TAC last, all on stdout.
const stripTac = (out, tac) => (tac && out.endsWith(tac) ? out.slice(0, out.length - tac.length) : out);

router.post('/', async (req, res, next) => {
  const code = req.body && req.body.code;
  if (typeof code !== 'string' || !code.trim()) return next(new HttpError(400, 'Source code is required.'));
  if (code.length > 100000) return next(new HttpError(413, 'Source code too large.'));

  const file = path.join(os.tmpdir(), `lexify-${crypto.randomUUID()}.mc`);
  try {
    await fs.writeFile(file, code);

    const main = await run(file);
    if (main.err && main.err.code === 'ENOENT') throw new HttpError(500, 'Compiler binary not found. Build it with `make` or set LEXIFY_BIN.');
    if (main.err && main.err.killed) throw new HttpError(500, 'Compiler timed out.');

    countRun(req);

    // Lexical / syntax / semantic errors -> still HTTP 200 with ok:false (see API.md)
    if (main.err) {
      const errors = main.stderr.split('\n').map(s => s.trim()).filter(Boolean);
      return res.json({ ok: false, errors: errors.length ? errors : ['Compilation failed.'] });
    }

    const [astRun, symRun] = await Promise.all([run(file, '--ast'), run(file, '--symtab')]);
    res.json({
      ok: true,
      tac: main.stdout,
      ast: stripTac(astRun.stdout, main.stdout),
      symtab: stripTac(symRun.stdout, main.stdout),
      errors: [],
    });
  } catch (e) {
    next(e);
  } finally {
    fs.unlink(file).catch(() => {});
  }
});

module.exports = router;
