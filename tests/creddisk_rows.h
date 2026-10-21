/* creddisk_rows.h: the report rows of each credential persistence boot. */
#ifndef CREDDISK_ROWS_H
#define CREDDISK_ROWS_H

static const char *const creddisk_write_rows[] = {
    "write-begin", "write-publish", "write-release", "write-begin-held", "write-publish-held", 0};

static const char *const creddisk_read_rows[] = {"read-no-temps",
                                                 "read-released-bytes",
                                                 "read-held-bytes",
                                                 "read-held-lock",
                                                 "read-capture",
                                                 "read-refused",
                                                 "read-lock-kept",
                                                 "read-removal-advice",
                                                 "read-remove-lock",
                                                 "read-reacquire",
                                                 "read-republish",
                                                 "read-release",
                                                 0};

static const char *const creddisk_final_rows[] = {"final-no-lock", "final-no-temps",
                                                  "final-released-bytes", 0};

static const char *const *const creddisk_rows[] = {creddisk_write_rows, creddisk_read_rows,
                                                   creddisk_final_rows};

#endif
