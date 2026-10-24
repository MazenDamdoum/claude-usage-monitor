'use strict';
// Console + rotating file log. Callers must never pass secrets (tokens, keys,
// subscription endpoints); use redactEndpoint() for push endpoints.

const fs = require('fs');
const path = require('path');

const MAX_BYTES = 512 * 1024;

function createLogger(dataDir, { quiet = false } = {}) {
  const file = path.join(dataDir, 'server.log');

  function write(level, message) {
    const line = `${new Date().toISOString()}  ${level.padEnd(5)}  ${message}`;
    if (!quiet) (level === 'ERROR' ? console.error : console.log)(line);
    try {
      if (fs.existsSync(file) && fs.statSync(file).size > MAX_BYTES) {
        fs.renameSync(file, `${file}.1`);
      }
      fs.appendFileSync(file, line + '\n');
    } catch {
      // logging must never take the server down
    }
  }

  return {
    info: (m) => write('INFO', m),
    warn: (m) => write('WARN', m),
    error: (m) => write('ERROR', m),
  };
}

function redactEndpoint(endpoint) {
  try {
    return new URL(endpoint).host;
  } catch {
    return '(invalid endpoint)';
  }
}

module.exports = { createLogger, redactEndpoint };
