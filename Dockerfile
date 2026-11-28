# Lexify (compiler + website) for Render / any Docker host
FROM node:22-bookworm-slim

# gcc + flex + bison: build the compiler on Linux (never use a Windows .exe)
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential flex bison \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN rm -rf build lexify lexify.exe && make && make test

WORKDIR /app/prototypes/backend
RUN npm ci --omit=dev

ENV NODE_ENV=production \
    LEXIFY_BIN=/app/lexify \
    FRONTEND_DIR=/app/prototypes/frontend \
    DB_PATH=/app/prototypes/backend/data/lexify.db \
    SESSION_TTL_HOURS=24 \
    PORT=3000
EXPOSE 3000
CMD ["node", "src/server.js"]
