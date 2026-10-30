const express = require('express');
const db = require('../db');
const { HttpError, LANGUAGES } = require('../utils');
const { requireAuth } = require('../middleware/auth');

const router = express.Router();
router.use(requireAuth);

const MAX_CODE = 500 * 1000; // characters

const cols = 'id, file_name AS fileName, language, created_at AS createdAt';
const list = db.prepare(`SELECT ${cols} FROM downloads WHERE user_id = ? ORDER BY id DESC LIMIT 200`);
const insert = db.prepare('INSERT INTO downloads (user_id, file_name, language, code) VALUES (?, ?, ?, ?)');
const one = db.prepare(`SELECT ${cols} FROM downloads WHERE id = ? AND user_id = ?`);
const oneWithCode = db.prepare(`SELECT ${cols}, code FROM downloads WHERE id = ? AND user_id = ?`);
const remove = db.prepare('DELETE FROM downloads WHERE id = ? AND user_id = ?');

const idOf = req => {
  const id = Number(req.params.id);
  if (!Number.isInteger(id) || id < 1) throw new HttpError(404, 'Download not found.');
  return id;
};

// GET /downloads -> { downloads: [{ id, fileName, language, createdAt }] }
router.get('/', (req, res) => res.json({ downloads: list.all(req.user.id) }));

// POST /downloads { fileName, language, code } -> 201 { download }
router.post('/', (req, res) => {
  const { fileName, language, code } = req.body || {};
  const errors = {};
  const name = typeof fileName === 'string' ? fileName.trim().slice(0, 200) : '';
  if (!name) errors.fileName = 'fileName is required.';
  if (!LANGUAGES.includes(language)) errors.language = 'language must be C, C++ or Java.';
  if (code !== undefined && typeof code !== 'string') errors.code = 'code must be text.';
  if (typeof code === 'string' && code.length > MAX_CODE) errors.code = 'Code is too large.';
  if (Object.keys(errors).length) throw new HttpError(422, 'fileName and language are required.', errors);

  const info = insert.run(req.user.id, name, language, code || '');
  res.status(201).json({ download: one.get(info.lastInsertRowid, req.user.id) });
});

// GET /downloads/:id -> { download: { ..., code } }
router.get('/:id', (req, res) => {
  const row = oneWithCode.get(idOf(req), req.user.id);
  if (!row) throw new HttpError(404, 'Download not found.');
  res.json({ download: row });
});

// DELETE /downloads/:id -> { ok: true }
router.delete('/:id', (req, res) => {
  if (!remove.run(idOf(req), req.user.id).changes) throw new HttpError(404, 'Download not found.');
  res.json({ ok: true });
});

module.exports = router;
