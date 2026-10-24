// node:sqlite implementation of the Edge Db (tests and the cross-check CLI).
import { DatabaseSync } from "node:sqlite";
import type { Db, Row, SqlValue } from "../src/core/db.ts";

export class NodeDb implements Db {
  readonly raw: DatabaseSync;
  failNextCommit = false;     // fault injection: COMMIT throws once

  constructor(path = ":memory:") {
    this.raw = new DatabaseSync(path);
    if (path !== ":memory:") this.raw.exec("PRAGMA journal_mode=WAL");
    this.raw.exec("PRAGMA synchronous=FULL");
  }
  async execute(sql: string): Promise<void> { this.raw.exec(sql); }
  async run(sql: string, params: SqlValue[] = []): Promise<void> {
    this.raw.prepare(sql).run(...params);
  }
  async query(sql: string, params: SqlValue[] = []): Promise<Row[]> {
    return this.raw.prepare(sql).all(...params) as Row[];
  }
  async transaction<T>(fn: () => Promise<T>): Promise<T> {
    this.raw.exec("BEGIN IMMEDIATE");
    try {
      const out = await fn();
      if (this.failNextCommit) { this.failNextCommit = false; throw new Error("injected"); }
      this.raw.exec("COMMIT");
      return out;
    } catch (e) {
      this.raw.exec("ROLLBACK");
      throw e;
    }
  }
}
