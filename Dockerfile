FROM python:3.14-alpine

WORKDIR /app
COPY buddy_recv.py /app/buddy_recv.py

RUN chmod +x /app/buddy_recv.py

EXPOSE 8001

ENV BUDDY_RECV_HOST=0.0.0.0 \
    BUDDY_RECV_PORT=8001 \
    BAMBUDDY_URL=http://host.docker.internal:8000

CMD ["python", "/app/buddy_recv.py"]
