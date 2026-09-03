FROM registry:2

RUN apk add --no-cache openssl

# Fixed paths so the entrypoint can generate the cert idempotently and the
# stock registry entrypoint picks it up via these same env vars.
ENV REGISTRY_HTTP_TLS_CERTIFICATE=/certs/registry.crt
ENV REGISTRY_HTTP_TLS_KEY=/certs/registry.key

COPY entrypoint.sh /entrypoint-wrapper.sh
RUN chmod +x /entrypoint-wrapper.sh

ENTRYPOINT ["/entrypoint-wrapper.sh"]
CMD ["/etc/docker/registry/config.yml"]
