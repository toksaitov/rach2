/* Keep the serial queue full after a witnessed native child exit. */
struct pressure_result {
    char out[9000], err[64];
    int exited, streams[2];
};

static int pressure_frame(struct pressure_result *result, const struct got *frame,
                          const char *command, const char *token)
{
    if (!strcmp(frame->type, ETUDE_CMD_ACK))
        return 0;
    if (frame->nfields < 2 || strcmp(frame->field[0], command) || strcmp(frame->field[1], token))
        return -1;
    if (!strcmp(frame->type, ETUDE_EV_OUT)) {
        unsigned char bytes[8192];
        int err = !strcmp(frame->field[2], "2");
        char *out = err ? result->err : result->out;
        size_t capacity = err ? sizeof result->err : sizeof result->out;
        long count = wire_unbase64(frame->field[4], bytes);
        size_t offset = strtoul(frame->field[3], NULL, 10);
        if (count < 0 || offset != strlen(out) || (size_t)count >= capacity - offset)
            return -1;
        memcpy(out + offset, bytes, (size_t)count);
        out[offset + (size_t)count] = 0;
    } else if (!strcmp(frame->type, ETUDE_EV_EXIT)) {
        result->exited = !strcmp(frame->field[2], "exited") && !strcmp(frame->field[3], "0");
    } else if (!strcmp(frame->type, ETUDE_EV_STREAM)) {
        int fd = !strcmp(frame->field[2], "2");
        result->streams[fd] = stream_agrees(frame->field + 3, fd ? result->err : result->out);
    } else if (!strcmp(frame->type, ETUDE_EV_CLEANED)) {
        return result->exited && result->streams[0] && result->streams[1] &&
                       !strcmp(frame->field[2], "confirmed")
                   ? 1
                   : -1;
    }
    return 0;
}

/** Read the attempt's record until it says the attempt ended, or until the
 * guest exits, and check that the stop left it stopped and unconfirmed. */
static void pressure_stopped(const char *token)
{
    char path[800], text[4096] = {0};
    snprintf(path, sizeof path, "%s/state/a/%s/record", work, token);
    for (;;) {
        int gone = waitpid(guest, NULL, WNOHANG) == guest;
        FILE *file = fopen(path, "rb");
        size_t n = file ? fread(text, 1, sizeof text - 1, file) : 0;
        text[n] = 0;
        if (file)
            fclose(file);
        if (gone)
            guest = -1;
        if (gone || strstr(text, "\nstate ended\n"))
            break;
        struct got discarded;
        next_frame(&discarded, 20);
    }
    CHECK(strstr(text, "\nstate ended\n") && strstr(text, " stopped -\n") &&
              strstr(text, " unconfirmed\n"),
          "stopping a full exited queue records stopped and unconfirmed without ACKs");
}

static void check_backpressure_row(int stop)
{
    const char *token = stop == 2 ? "000000000000f0b3"
                        : stop    ? "000000000000f0b2"
                                  : "000000000000f0b1";
    struct pressure_result result = {0};
    struct got frame;
    char invocation[65], command[24], witness;
    int fd = watch("marks.pressure"), status = 0, prefix = 0;
    CHECK(fd >= 0, "the child-exit witness FIFO is open before launch");
    start_guest(NULL, "--resend-ms", "50", (char *)NULL);
    serial_acks = 1;
    seen = 0;
    hello(ETUDE_FRAME_SERIAL_MAX);
    put_case("backpressure", "guest-backpressure", token, "ordinary", stop == 2 ? 500 : 30000,
             invocation);
    send_cmd(ETUDE_CMD_RUN, new_cmd(command), token, invocation, (char *)NULL);
    while (!prefix && next_frame(&frame, -1) == 0) {
        status = pressure_frame(&result, &frame, command, token);
        if (status)
            break;
        prefix = strlen(result.out) > 4096;
    }
    CHECK(prefix && !status, "the responder queued output from a second pipe read");
    CHECK(fd >= 0 && read(fd, &witness, 1) == 1 && witness == 'r',
          "the actual child opened and wrote its native exit witness");
    CHECK(all_gone(fd), "the child closed its witness at process exit");

    /* Withhold ACKs past the original two-second post-exit drain. This
     * wait tests that interval; it is not a readiness delay. */
    serial_acks = 0;
    long long until = deadline(2200);
    while (left(until) > 0)
        next_frame(&frame, left(until));
    if (stop) {
        if (stop == 1)
            send_cmd(ETUDE_CMD_CANCEL, new_cmd(command), token, (char *)NULL);
        pressure_stopped(token);
        end_guest();
        serial_acks = 0;
        return;
    }
    serial_acks = 1;
    while (!status && next_frame(&frame, -1) == 0)
        status = pressure_frame(&result, &frame, command, token);
    CHECK(status == 1, "the complete streams precede confirmed cleanup");
    CHECK(strlen(result.out) == 8206 && strspn(result.out, "0") == 8192 &&
              !strcmp(result.out + 8192, "\nFINAL OUTPUT\n") &&
              !strcmp(result.err, "FINAL ERROR\n"),
          "every final byte arrives after serial backpressure ends");
    send_cmd(ETUDE_CMD_BYE, new_cmd(command), (char *)NULL);
    CHECK(expect(&frame, ETUDE_CMD_BYE) == 0, "the serial session accepts bye");
    end_guest();
    serial_acks = 0;
}

#if defined(F_SETPIPE_SZ)
/** Read the console through a pipe of one page, a frame every 40 ms, as
 * slowly as a loaded host's polled UART sends it. Each write of a frame then
 * waits for the reader, as Rach2's serial console does, and sending what
 * the child left after its exit takes longer than the drain allowance. */
static void check_slow_console(void)
{
    const char *token = "000000000000f0b4";
    struct pressure_result result = {0};
    struct got frame;
    char invocation[65], command[24], witness;
    int fd = watch("marks.pressure"), status = 0, prefix = 0;
    CHECK(fd >= 0, "the child-exit witness FIFO is open before launch");
    start_guest(NULL, "--resend-ms", "60000", (char *)NULL);
    CHECK(fcntl(from_guest, F_SETPIPE_SZ, 4096) == 4096, "the console pipe holds one page");
    serial_acks = 1;
    seen = 0;
    hello(ETUDE_FRAME_SERIAL_MAX);
    put_case("slowconsole", "guest-backpressure", token, "ordinary", 30000, invocation);
    send_cmd(ETUDE_CMD_RUN, new_cmd(command), token, invocation, (char *)NULL);
    while (!prefix && next_frame(&frame, -1) == 0) {
        status = pressure_frame(&result, &frame, command, token);
        if (status)
            break;
        prefix = strlen(result.out) > 0;
    }
    CHECK(prefix && !status, "the responder sent the first output");
    CHECK(fd >= 0 && read(fd, &witness, 1) == 1 && witness == 'r',
          "the actual child opened and wrote its native exit witness");
    CHECK(all_gone(fd), "the child closed its witness at process exit");
    while (!status && next_frame(&frame, -1) == 0) {
        status = pressure_frame(&result, &frame, command, token);
        /* interval: the pace of a slow serial line, one frame per 40 ms. */
        poll(NULL, 0, 40);
    }
    CHECK(status == 1, "the complete streams precede confirmed cleanup on a slow console");
    CHECK(strlen(result.out) == 8206 && strspn(result.out, "0") == 8192 &&
              !strcmp(result.out + 8192, "\nFINAL OUTPUT\n") &&
              !strcmp(result.err, "FINAL ERROR\n"),
          "every final byte arrives over a console slower than the drain allowance");
    send_cmd(ETUDE_CMD_BYE, new_cmd(command), (char *)NULL);
    CHECK(expect(&frame, ETUDE_CMD_BYE) == 0, "the serial session accepts bye");
    end_guest();
    serial_acks = 0;
}
#endif

static void check_backpressure(void)
{
    check_backpressure_row(0);
    check_backpressure_row(1);
    check_backpressure_row(2);
#if defined(F_SETPIPE_SZ)
    check_slow_console();
#endif
}
