const crypto = require('crypto');

const newToken = () => crypto.randomBytes(32).toString('hex');
const hashToken = t => crypto.createHash('sha256').update(t).digest('hex');

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/;
const LANGUAGES = ['C', 'C++', 'Java'];

const validateEmail = email =>
  typeof email === 'string' && email.length <= 254 && EMAIL_RE.test(email.trim());

function validateName(name) {
  if (typeof name !== 'string' || !name.trim()) return 'Please enter your name.';
  if (name.trim().length > 80) return 'Name must be at most 80 characters.';
  return null;
}

// Signup rule (matches signup.html): min 6. bcrypt only reads the first 72 bytes.
function validateSignupPassword(pw) {
  if (typeof pw !== 'string' || !pw) return 'Please enter a password.';
  if (pw.length < 6) return 'Password must be at least 6 characters.';
  if (Buffer.byteLength(pw) > 72) return 'Password must be at most 72 bytes.';
  return null;
}

// Change-password rule (matches account.html): min 8.
function validateNewPassword(pw) {
  if (typeof pw !== 'string' || !pw) return 'Please enter a new password.';
  if (pw.length < 8) return 'New password must be at least 8 characters.';
  if (Buffer.byteLength(pw) > 72) return 'Password must be at most 72 bytes.';
  return null;
}

// API.md error format: { message, errors?: { field: message } }
class HttpError extends Error {
  constructor(status, message, errors) {
    super(message);
    this.status = status;
    this.errors = errors;
  }
}

// API.md user shape: { id, name, email, lang, runs, files }
const publicUser = (u, files = 0) => ({
  id: u.id, name: u.name, email: u.email, lang: u.lang, runs: u.runs, files,
});

module.exports = {
  newToken, hashToken, validateEmail, validateName,
  validateSignupPassword, validateNewPassword,
  HttpError, publicUser, LANGUAGES,
};
