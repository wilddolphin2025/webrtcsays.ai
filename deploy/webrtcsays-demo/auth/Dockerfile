FROM python:3.12-slim

RUN useradd --system --uid 8793 --no-create-home --shell /usr/sbin/nologin demoauth \
    && mkdir -p /app \
    && chown demoauth:demoauth /app

COPY demo_auth.py /app/demo_auth.py

USER demoauth
WORKDIR /app

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    DEMO_BIND=0.0.0.0 \
    DEMO_PORT=8793 \
    DEMO_CONF_DIR=/conf \
    DEMO_TRUST_PROXY=1

EXPOSE 8793

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD ["python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8793/healthz', timeout=2).read()"]

ENTRYPOINT ["python", "-u", "/app/demo_auth.py"]
