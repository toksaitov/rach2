# Etude

Etude's history format records the identity, work, time and result of a test run. The schemas define the records that the runner and its reports will exchange; the runner and collectors are not implemented yet.

## Interfaces

[`schemas/history.schema.json`](schemas/history.schema.json) defines version 1 of the JSON Lines history. [`schemas/history.example.jsonl`](schemas/history.example.jsonl) gives synthetic records, including an unavailable measurement and comparisons with insufficient or incompatible data. [`schemas/history.incomplete.example.jsonl`](schemas/history.incomplete.example.jsonl) shows a lost connection with unconfirmed cleanup and an unknown budget result. Each line is one record.

JSON Schema checks the shape of a record. Readers must also validate identities, referenced content, completeness and comparison eligibility before using a record as evidence.

## Limits

These files define a storage contract. No command writes history, validates relationships between records or selects a baseline yet. A schema-valid record alone does not establish a completed test or a valid comparison.
