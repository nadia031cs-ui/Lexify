const express = require('express');
const cors = require('cors');
const rateLimit = require('express-rate-limit');
const config = require('./config');
const db = require('./db');
const { HttpError } = require('./utils');

const app = express();
app.disable('x-powered-by');

app.use(cors({
  origin: config.corsOrigins.includes('*') ? true : config.corsOrigins,
  methods: ['GET', 'POST', 'PUT', 'DELETE', 'OPTIONS'],
  allowedHeaders: ['Content-Type', 'Authorization'],
}));
app.use(express.json({ limit: '1mb' })); // downloads store the code, so allow more than 100kb

app.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'DENY');
  res.setHeader('Referrer-Policy', 'no-referrer');
  next();
});

// Brute-force protection on signup/login/logout
const authLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  limit: process.env.NODE_ENV === 'test' ? 1000 : 50,
  standardHeaders: true,
  legacyHeaders: false,
  message: { message: 'Too many attempts, try again later.' },
});

app.get('/api/health', (req, res) => res.json({ ok: true }));
app.use('/api/auth', authLimiter, require('./routes/auth'));
app.use('/api/user', require('./routes/user'));
app.use('/api/downloads', require('./routes/downloads'));
app.use('/api', (req, res) => res.status(404).json({ message: 'Not found.' }));

// Serve the website from the same server (no CORS needed): set FRONTEND_DIR in .env
if (config.frontendDir) app.use(express.static(config.frontendDir));

app.use((err, req, res, next) => {
  if (err instanceof HttpError) {
    return res.status(err.status).json({ message: err.message, ...(err.errors ? { errors: err.errors } : {}) });
  }
  if (err.type === 'entity.parse.failed') return res.status(400).json({ message: 'Invalid JSON.' });
  if (err.type === 'entity.too.large') return res.status(413).json({ message: 'Request too large.' });
  console.error(err);
  res.status(500).json({ message: 'Server error.' });
});

const cleanup = () => db.prepare('DELETE FROM sessions WHERE expires_at < ?').run(Date.now());
cleanup();
setInterval(cleanup, 60 * 60 * 1000).unref();

module.exports = app;
