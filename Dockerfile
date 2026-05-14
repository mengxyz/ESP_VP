FROM node:22-alpine AS ui-builder

WORKDIR /app/esp-vp/ui
RUN corepack enable
COPY ui/package.json ui/pnpm-lock.yaml* ./
RUN pnpm install --frozen-lockfile=false
COPY ui/ ./
RUN pnpm run build

FROM python:3.14-alpine

WORKDIR /app
COPY buddy_recv.py /app/buddy_recv.py
COPY --from=ui-builder /app/esp-vp/ui/dist /app/ui/dist

RUN pip install --no-cache-dir \
    fastapi \
    "uvicorn[standard]" \
    httpx \
    python-multipart \
    cryptography

RUN chmod +x /app/buddy_recv.py

EXPOSE 8001

ENV BUDDY_RECV_HOST=0.0.0.0 \
    BUDDY_RECV_PORT=8001 \
    BUDDY_RECV_DATA_DIR=/data \
    BAMBUDDY_URL=http://host.docker.internal:8000

CMD ["python", "/app/buddy_recv.py"]
