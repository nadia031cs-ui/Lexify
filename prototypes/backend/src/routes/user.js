const express = require('express');
const bcrypt = require('bcryptjs');
const db = require('../db');
const config = require('../config');
const { validateEmail, validateName, validateNewPassword, HttpError, publicUser, LANGUAGES } = require('../utils');
const { requireAuth } = require('../middleware/auth');

const router = express.Router();
router.use(requireAuth);

const getUser = db.prepare('SELECT * FROM users WHERE id = ?');
const countFiles = db.prepare('SELECT COUNT(*) AS n FROM downloads WHERE user_id = ?');
const emailTaken = db.prepare('SELECT id FROM users WHERE email = ? AND id != ?');
const updateProfile = db.prepare("UPDATE users SET name = ?, email = ?, lang = ?, updated_at = datetime('now') WHERE id = ?");
const updatePassword = db.prepare("UPDATE users SET password_hash = ?, updated_at = datetime('now') WHERE id = ?");
const killOtherSessions = db.prepare('DELETE FROM sessions WHERE user_id = ? AND id != ?');

const fresh = id => publicUser(getUser.get(id), countFiles.get(id).n);

// GET /user/me -> { user }
router.get('/me', (req, res) => res.json({ user: fresh(req.user.id) }));

// PUT /user/profile { name, email, lang } -> { user }   (fields you leave out keep their old value)
router.put('/profile', (req, res) => {
  const body = req.body || {};
  const cur = req.user;
  const name = body.name === undefined ? cur.name : body.name;
  const email = body.email === undefined ? cur.email : body.email;
  const lang = body.lang === undefined ? cur.lang : body.lang;

  const errors = {};
  const nameErr = validateName(name);
  if (nameErr) errors.name = nameErr;
  if (!validateEmail(email)) errors.email = 'Please enter a valid email address.';
  if (!LANGUAGES.includes(lang)) errors.lang = 'Unsupported language.';
  if (Object.keys(errors).length) throw new HttpError(422, 'Please fix the errors.', errors);

  const cleanEmail = email.trim().toLowerCase();
  if (emailTaken.get(cleanEmail, cur.id)) {
    throw new HttpError(422, 'Please fix the errors.', { email: 'This email is already used by another account.' });
  }
  updateProfile.run(name.trim(), cleanEmail, lang, cur.id);
  res.json({ user: fresh(cur.id) });
});

// PUT /user/password { currentPassword, newPassword } -> { ok: true }
router.put('/password', (req, res) => {
  const { currentPassword, newPassword } = req.body || {};
  if (typeof currentPassword !== 'string' || !bcrypt.compareSync(currentPassword, req.user.password_hash)) {
    throw new HttpError(400, 'Current password is incorrect.');
  }
  const pwErr = validateNewPassword(newPassword);
  if (pwErr) throw new HttpError(422, pwErr);
  if (newPassword === currentPassword) throw new HttpError(422, 'New password must be different.');

  updatePassword.run(bcrypt.hashSync(newPassword, config.bcryptRounds), req.user.id);
  killOtherSessions.run(req.user.id, req.sessionId); // log out other devices, keep this one
  res.json({ ok: true });
});

module.exports = router;
