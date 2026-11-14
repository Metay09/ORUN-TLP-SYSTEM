import assert from "node:assert/strict";
import { test } from "node:test";
import { WriteKeyBook, type WriteKey } from "../src/app/writeKeys.ts";

const DEVICE = "0123456789ABCDEF";
const OTHER_DEVICE = "FEDCBA9876543210";
const FIRST = "12".repeat(32);
const SECOND = "34".repeat(32);

function createStore() {
  const values = new Map<string, string>();
  return {
    values,
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value); },
    removeItem: (key: string) => { values.delete(key); },
  };
}

test("clear removes persisted signing authority", async () => {
  const store = createStore();
  const book = new WriteKeyBook(store);
  await book.refresh({ writeKeys: async () => [{ device: DEVICE, epoch: 1, writeKey: FIRST }] });
  assert.equal(book.size, 1);
  assert.equal(store.values.has("orun.writeKeys.v1"), true);
  book.clear();
  assert.equal(book.size, 0);
  assert.equal(book.get(DEVICE, 1), null);
  assert.equal(store.values.has("orun.writeKeys.v1"), false);
});

test("an old account's delayed refresh cannot restore keys after clear", async () => {
  const store = createStore();
  const book = new WriteKeyBook(store);
  let finishOld!: (keys: WriteKey[]) => void;
  const oldFetch = new Promise<WriteKey[]>((resolve) => { finishOld = resolve; });
  const staleRefresh = book.refresh({ writeKeys: () => oldFetch });
  book.clear();
  const newRefresh = await book.refresh({
    writeKeys: async () => [{ device: OTHER_DEVICE, epoch: 2, writeKey: SECOND }],
  });
  assert.equal(newRefresh, 1);
  finishOld([{ device: DEVICE, epoch: 1, writeKey: FIRST }]);
  assert.equal(await staleRefresh, 0);
  assert.equal(book.get(DEVICE, 1), null);
  assert.equal(book.active(DEVICE), null);
  assert.equal(book.size, 1);
  assert.equal(Buffer.from(book.get(OTHER_DEVICE, 2)!).toString("hex").toUpperCase(), SECOND);
  assert.equal(JSON.parse(store.values.get("orun.writeKeys.v1")!)[0].device, OTHER_DEVICE);
});
