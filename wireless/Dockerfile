FROM python:3.13-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY wireless/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt
COPY wireless /app/wireless
RUN useradd --create-home --uid 10001 card && mkdir /data && chown card /data
USER card
ENV CARD_DATA_DIR=/data
EXPOSE 8787 1883
CMD ["python", "-m", "wireless.server"]
