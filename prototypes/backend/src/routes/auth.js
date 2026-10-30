const express = require('express');
const bcrypt = require('bcryptjs');
const db = require('../db');
const config = require('../config');
const { newToken, hashToken, validateEmail, validateName, validateSignupPassword, HttpError, publicUser } = require('../utils');
const { requireAuth } = require('../middleware/auth');

const router = express.Router();

const insertUser = db.prepare('INSERT INTO users (name, email, password_hash) VALUES (?, ?, ?)');
const userByEmail = db.prepare('SELECT * FROM users WHERE email = ?');
const userById = db.prepare('SELECT * FROM users WHERE id = ?');
const countFiles = db.prepare('SELECT COUNT(*) AS n FROM downloads WHERE user_id = ?');
const insertSession = db.prepare('INSERT INTO sessions (user_id, token_hash, expires_at) VALUES (?, ?, ?)');
const deleteSession = db.prepare('DELETE FROM sessions WHERE id = ?');
const deleteAllSessions = db.prepare('DELETE FROM sessions WHERE user_id = ?');

// Compared against when the email doesn't exist, so timing doesn't reveal which emails are registered
const DUMMY_HASH = bcrypt.hashSync('dummy-password', config.bcryptRounds);

// POST /auth/signup  -> 201 { user }   (422 field errors, 409 email taken)
router.post('/signup', (req, res) => {
  const { name, email, password } = req.body || {};
  const errors = {};
  const nameErr = validateName(name);
  if (nameErr) errors.name = nameErr;
  if (!validateEmail(email)) errors.email = 'Please enter a valid email.';
  const pwErr = validateSignupPassword(password);
  if (pwErr) errors.password = pwErr;
  if (Object.keys(errors).length) throw new HttpError(422, 'Please fix the errors.', errors);

  const cleanEmail = email.trim().toLowerCase();
  if (userByEmail.get(cleanEmail)) {
    throw new HttpError(409, 'This email is already registered.', { email: 'This email is already registered.' });
  }
  const info = insertUser.run(name.trim(), cleanEmail, bcrypt.hashSync(password, config.bcryptRounds));
  res.status(201).json({ user: publicUser(userById.get(info.lastInsertRowid), 0) });
});

// POST /auth/login  -> 200 { token, user }   (401 wrong email/password)
router.post('/login', (req, res) => {
  const { email, password } = req.body || {};
  if (typeof email !== 'string' || typeof password !== 'string' || !email || !password) {
    throw new HttpError(401, 'Incorrect email or password.');
  }
  const user = userByEmail.get(email.trim().toLowerCase());
  const ok = bcrypt.compareSync(password, user ? user.password_hash : DUMMY_HASH);
  if (!user || !ok) throw new HttpError(401, 'Incorrect email or password.');

  const token = newToken();
  insertSession.run(user.id, hashToken(token), Date.now() + config.sessionTtlMs);
  res.json({ token, user: publicUser(user, countFiles.get(user.id).n) });
});

// POST /auth/logout  -> 200 { ok: true }
router.post('/logout', requireAuth, (req, res) => {
  deleteSession.run(req.sessionId);
  res.json({ ok: true });
});

// Extra: sign out on every device
router.post('/logout-all', requireAuth, (req, res) => {
  deleteAllSessions.run(req.user.id);
  res.json({ ok: true });
});

module.exports = router;
