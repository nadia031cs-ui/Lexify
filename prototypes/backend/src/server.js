const app = require('./app');
const config = require('./config');

app.listen(config.port, () => {
  console.log(`Lexify backend running on http://localhost:${config.port}`);
});
