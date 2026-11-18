import assert from "node:assert/strict";

import { Configuration, RoomsApi } from "./generated/index.js";

const [baseUrl] = process.argv.slice(2);
assert.ok(baseUrl, "请传入服务的 HTTP 基础地址");
const address = new URL(baseUrl);
assert.ok(["http:", "https:"].includes(address.protocol));
const statuses: number[] = [];
const api = new RoomsApi(new Configuration({
  basePath: address.href.replace(/\/+$/, ""),
  fetchApi: async (input, init) => {
    const response = await fetch(input, init);
    statuses.push(response.status);
    return response;
  },
}));
const room = { room_name: "契约测试房间", capacity: 42 };

const created = await api.createRoom(room);
assert.ok(Number.isInteger(created.id) && created.id > 0);
const expectedRoom = { id: created.id, ...room };
assert.deepEqual(created, expectedRoom);

assert.deepEqual(await api.getRoom(created.id), expectedRoom);

const listed = await api.listRooms();
assert.deepEqual(statuses, [201, 200, 200]);
assert.ok(Array.isArray(listed.rooms));
assert.equal(listed.total, listed.rooms.length);
assert.deepEqual(listed.rooms.filter((item) => item.id === created.id), [expectedRoom]);

const [create, query, list] = statuses;
console.log(JSON.stringify({ create, query, list, room: expectedRoom, total: listed.total }));
