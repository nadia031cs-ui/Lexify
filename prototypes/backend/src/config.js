require('dotenv').config();
const path = require('path');

module.exports = {
  port: Number(process.env.PORT) || 3000,
  corsOrigins: (process.env.CORS_ORIGINS || '*').split(',').map(s => s.trim()).filter(Boolean),
  sessionTtlMs: (Number(process.env.SESSION_TTL_HOURS) || 24) * 60 * 60 * 1000,
  dbPath: path.resolve(process.env.DB_PATH || './data/lexify.db'),
  frontendDir: process.env.FRONTEND_DIR ? path.resolve(process.env.FRONTEND_DIR) : null,
  bcryptRounds: 10,
};
