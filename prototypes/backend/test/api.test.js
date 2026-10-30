// End-to-end test against API.md: signup -> login -> me -> profile -> password -> downloads -> logout
process.env.NODE_ENV = 'test';
process.env.DB_PATH = require('path').join(require('os').tmpdir(), `lexify-test-${Date.now()}.db`);
const assert = require('assert');
const app = require('../src/app');

const server = app.listen(0, async () => {
  const base = `http://127.0.0.1:${server.address().port}/api`;
  const call = async (method, path, body, token) => {
    const res = await fetch(base + path, {
      method,
      headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: `Bearer ${token}` } : {}) },
      body: body ? JSON.stringify(body) : undefined,
    });
    return { status: res.status, data: await res.json().catch(() => ({})) };
  };
  let n = 0;
  const ok = name => console.log(`  ok ${++n} - ${name}`);

  try {
    let r = await call('POST', '/auth/signup', { name: '', email: 'bad', password: '123' });
    assert.equal(r.status, 422);
    assert.ok(r.data.message && r.data.errors.name && r.data.errors.email && r.data.errors.password);
    ok('signup validation (422, { message, errors })');

    r = await call('POST', '/auth/signup', { name: 'Nadia Test', email: 'Nadia@Example.com', password: 'abc123' });
    assert.equal(r.status, 201);
    assert.deepEqual(r.data.user, { id: 1, name: 'Nadia Test', email: 'nadia@example.com', lang: 'C', runs: 0, files: 0 });
    assert.equal(JSON.stringify(r.data).includes('hash'), false);
    ok('signup with 6-char password (201, { user }, no hash)');

    r = await call('POST', '/auth/signup', { name: 'Dup', email: 'nadia@example.com', password: 'abc123' });
    assert.equal(r.status, 409); assert.ok(r.data.errors.email); ok('duplicate email = 409');

    r = await call('POST', '/auth/login', { email: 'nadia@example.com', password: 'wrong' });
    assert.equal(r.status, 401); assert.equal(r.data.message, 'Incorrect email or password.'); ok('wrong password = 401');
    r = await call('POST', '/auth/login', { email: 'nobody@example.com', password: 'abc123' });
    assert.equal(r.status, 401); ok('unknown email = 401');

    r = await call('POST', '/auth/login', { email: 'NADIA@example.com', password: 'abc123' });
    assert.equal(r.status, 200); assert.equal(r.data.token.length, 64); assert.equal(r.data.user.name, 'Nadia Test');
    const token = r.data.token; ok('login (email case-insensitive)');

    r = await call('GET', '/user/me'); assert.equal(r.status, 401); ok('me without token = 401');
    r = await call('GET', '/user/me', null, token); assert.equal(r.status, 200); assert.equal(r.data.user.email, 'nadia@example.com'); ok('me');

    r = await call('PUT', '/user/profile', { name: 'Nadia M', email: 'nadia@example.com', lang: 'Java' }, token);
    assert.equal(r.status, 200); assert.equal(r.data.user.name, 'Nadia M'); assert.equal(r.data.user.lang, 'Java'); ok('profile update incl. lang');
    r = await call('PUT', '/user/profile', { name: 'Nadia M', email: 'nadia@example.com', lang: 'Rust' }, token);
    assert.equal(r.status, 422); assert.ok(r.data.errors.lang); ok('profile: bad lang = 422');

    await call('POST', '/auth/signup', { name: 'Other', email: 'other@example.com', password: 'abc123' });
    r = await call('PUT', '/user/profile', { name: 'Nadia M', email: 'other@example.com', lang: 'C' }, token);
    assert.equal(r.status, 422); assert.ok(r.data.errors.email); ok('profile: email used by another account = 422');

    r = await call('PUT', '/user/password', { currentPassword: 'nope', newPassword: 'newpass123' }, token);
    assert.equal(r.status, 400); ok('password: wrong current = 400');
    r = await call('PUT', '/user/password', { currentPassword: 'abc123', newPassword: 'short' }, token);
    assert.equal(r.status, 422); ok('password: new < 8 = 422');
    r = await call('PUT', '/user/password', { currentPassword: 'abc123', newPassword: 'abc123' }, token);
    assert.equal(r.status, 422); ok('password: same as current = 422');

    // second device, to prove other sessions get logged out on password change
    const dev2 = (await call('POST', '/auth/login', { email: 'nadia@example.com', password: 'abc123' })).data.token;
    r = await call('PUT', '/user/password', { currentPassword: 'abc123', newPassword: 'newpass123' }, token);
    assert.equal(r.status, 200); assert.equal(r.data.ok, true); ok('password change');
    r = await call('GET', '/user/me', null, dev2); assert.equal(r.status, 401); ok('other device logged out after password change');
    r = await call('GET', '/user/me', null, token); assert.equal(r.status, 200); ok('current session still valid');

    r = await call('POST', '/downloads', { fileName: 'x.c' }, token);
    assert.equal(r.status, 422); assert.ok(r.data.errors.language); ok('download: language required = 422');
    r = await call('POST', '/downloads', { fileName: 'main.c', language: 'C', code: 'int main(){return 0;}' }, token);
    assert.equal(r.status, 201); assert.equal(r.data.download.language, 'C'); assert.equal(r.data.download.fileName, 'main.c');
    const dl = r.data.download.id; ok('record download (201, { download })');
    r = await call('GET', '/user/me', null, token); assert.equal(r.data.user.files, 1); ok('me.files counts downloads');
    r = await call('GET', '/downloads', null, token); assert.equal(r.data.downloads.length, 1); ok('list downloads');
    r = await call('GET', `/downloads/${dl}`, null, token); assert.equal(r.data.download.code, 'int main(){return 0;}'); ok('get download with code');
    const otherTok = (await call('POST', '/auth/login', { email: 'other@example.com', password: 'abc123' })).data.token;
    r = await call('DELETE', `/downloads/${dl}`, null, otherTok); assert.equal(r.status, 404); ok("can't delete someone else's download");
    r = await call('DELETE', `/downloads/${dl}`, null, token); assert.equal(r.status, 200); ok('delete download');

    r = await call('POST', '/auth/logout', null, token); assert.deepEqual(r.data, { ok: true }); ok('logout');
    r = await call('GET', '/user/me', null, token); assert.equal(r.status, 401); ok('token dead after logout');
    r = await call('POST', '/auth/login', { email: 'nadia@example.com', password: 'newpass123' });
    assert.equal(r.status, 200); ok('login with new password');

    console.log(`\nAll ${n} checks passed`);
    server.close(() => process.exit(0));
  } catch (e) {
    console.error('FAILED:', e);
    server.close(() => process.exit(1));
  }
});
