const db = require('../db');
const { hashToken, HttpError } = require('../utils');

const find = db.prepare(`
  SELECT s.id AS session_id, s.expires_at, u.*
  FROM sessions s JOIN users u ON u.id = s.user_id
  WHERE s.token_hash = ?`);
const delSession = db.prepare('DELETE FROM sessions WHERE id = ?');

function requireAuth(req, res, next) {
  const m = (req.headers.authorization || '').match(/^Bearer\s+([a-f0-9]{64})$/i);
  if (!m) return next(new HttpError(401, 'Please log in.'));

  const row = find.get(hashToken(m[1]));
  if (!row) return next(new HttpError(401, 'Please log in.'));
  if (row.expires_at < Date.now()) {
    delSession.run(row.session_id);
    return next(new HttpError(401, 'Session expired, please log in again.'));
  }
  req.user = row;
  req.sessionId = row.session_id;
  next();
}

module.exports = { requireAuth };
