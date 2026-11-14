import assert from "node:assert/strict";
import { test } from "node:test";
import { WriteKeyBook, type WriteKey } from "../src/app/writeKeys.ts";

const DEVICE = "0123456789ABCDEF";
const OTHER_DEVICE = "FEDCBA9876543210";
const FIRST = "12".repeat(32);
const SECOND = "34".repeat(32);

function memoryVault() {
  let encryptedStore: WriteKey[] = [];
  return {
    vault: {
      load: async () => encryptedStore.map((k) => ({ ...k })),
      save: async (keys: WriteKey[]) => { encryptedStore = keys.map((k) => ({ ...k })); },
      clear: async () => { encryptedStore = []; },
    },
    stored: () => encryptedStore,
  };
}

test("native-vault abstraction persists offline keys without browser storage", async () => {
  const storage = memoryVault();
  const book = new WriteKeyBook(storage.vault);
  assert.equal(await book.refresh({
    writeKeys: async () => [{ device: DEVICE, epoch: 1, writeKey: FIRST }],
  }), 1);
  assert.equal(book.size, 1);
  const rebooted = new WriteKeyBook(storage.vault);
  await rebooted.load();
  assert.equal(rebooted.active(DEVICE)?.epoch, 1);
  assert.equal(Buffer.from(rebooted.get(DEVICE, 1)!).toString("hex").toUpperCase(), FIRST);
  await book.clear();
  const afterLogout = new WriteKeyBook(storage.vault);
  await afterLogout.load();
  assert.equal(afterLogout.size, 0);
  assert.deepEqual(storage.stored(), []);
});

test("old account delayed refresh cannot restore signing keys", async () => {
  const storage = memoryVault();
  const book = new WriteKeyBook(storage.vault);
  let finishOld!: (keys: WriteKey[]) => void;
  const delayed = new Promise<WriteKey[]>((resolve) => { finishOld = resolve; });
  const stale = book.refresh({ writeKeys: () => delayed });
  await book.clear();
  assert.equal(await book.refresh({
    writeKeys: async () => [{ device: OTHER_DEVICE, epoch: 2, writeKey: SECOND }],
  }), 1);
  finishOld([{ device: DEVICE, epoch: 1, writeKey: FIRST }]);
  assert.equal(await stale, 0);
  assert.equal(book.get(DEVICE, 1), null);
  assert.equal(book.size, 1);
  assert.equal(storage.stored()[0].device, OTHER_DEVICE);
});

test("a delayed vault read after logout cannot restore old write authority", async () => {
  let release!: (keys: WriteKey[]) => void;
  const waiting = new Promise<WriteKey[]>((resolve) => { release = resolve; });
  const vault = { load: () => waiting, save: async (_keys: WriteKey[]) => {}, clear: async () => {} };
  const book = new WriteKeyBook(vault);
  const reading = book.load();
  const clear = book.clear();
  release([{ device: DEVICE, epoch: 1, writeKey: FIRST }]);
  await Promise.all([reading, clear]);
  assert.equal(book.size, 0);
});

test("failed native persistence must not activate keys in RAM", async () => {
  const book = new WriteKeyBook({
    load: async () => [],
    save: async (_keys: WriteKey[]) => { throw new Error("keystore failed"); },
    clear: async () => {},
  });
  await assert.rejects(book.refresh({
    writeKeys: async () => [{ device: DEVICE, epoch: 1, writeKey: FIRST }],
  }), /keystore failed/);
  assert.equal(book.size, 0);
});

test("latest concurrent server list wins; stale response cannot revive an old epoch", async () => {
  const storage = memoryVault();
  const book = new WriteKeyBook(storage.vault);
  let finishOld!: (keys: WriteKey[]) => void;
  const slow = new Promise<WriteKey[]>((resolve) => { finishOld = resolve; });
  const first = book.refresh({ writeKeys: () => slow });
  await book.refresh({
    writeKeys: async () => [{ device: DEVICE, epoch: 2, writeKey: SECOND }],
  });
  finishOld([{ device: DEVICE, epoch: 1, writeKey: FIRST }]);
  assert.equal(await first, 0);
  assert.equal(book.get(DEVICE, 1), null);
  assert.equal(book.active(DEVICE)?.epoch, 2);
});
