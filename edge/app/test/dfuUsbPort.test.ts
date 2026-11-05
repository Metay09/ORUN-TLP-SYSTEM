import assert from "node:assert/strict";
import { test } from "node:test";
import { DfuUsbPort } from "../src/platform/dfuUsbPort.ts";
const id = "52E16773141A8887";
function fake(serial: string|null = id, pid = 0x0029) {
  let granted = false, closed = false;
  const history: string[] = [];
  const usb = {
    registerDriver: async (x: unknown) => {history.push("driver:"+JSON.stringify(x));},
    listDevices: async () => ({devices:[{
      deviceId:"one",vendorId:0x239a,productId:pid,driverType:"CdcAcmSerialDriver",deviceName:"RAK",portCount:1,
      serialNumber:serial,hasPermission:granted,
    }]}),
    requestPermission: async () => {granted=true;return {granted:true};},
    open: async () => ({portId:"port"}),
    getPortInfo: async () => ({deviceId:"one",portNum:0,serialNumber:serial,driverType:"CdcAcmSerialDriver"}),
    setParameters: async (x: {baudRate:number}) => {history.push("baud:"+x.baudRate);},
    setDTR: async (x: {value:boolean})=>{history.push("dtr:"+x.value);},
    write: async (x: {data:string})=>({bytesWritten:atob(x.data).length}),
    read: async ()=>({data:btoa(String.fromCharCode(0xc0,0x10,0,0,0xf0,0xc0))}),
    close: async ()=>{closed=true;},
  };
  return {usb,history,get closed(){return closed;}};
}

test("opens only matching bootloader and transfers raw bytes (no text corruption)",async()=>{
  const f=fake();const port=await DfuUsbPort.openBootloader(f.usb as never,id);
  assert.ok(f.history.includes("baud:115200"));
  assert.deepEqual(f.history.slice(-2),["dtr:false","dtr:true"]);
  await port.write(Uint8Array.of(0xc0,0,0xdb,0xff));
  assert.deepEqual(await port.read(200),Uint8Array.of(0xc0,0x10,0,0,0xf0,0xc0));
  await port.close();assert.equal(f.closed,true);
});
test("serial mismatch or missing identity fails closed before opening a port",async()=>{
  await assert.rejects(DfuUsbPort.openBootloader(fake("1111111111111111").usb as never,id),/farklı/);
  await assert.rejects(DfuUsbPort.openBootloader(fake(null).usb as never,id),/doğrulanamadı/);
});

test("alternate RAK bootloader USB PID 0x002A uses identical strict serial checks", async () => {
  const f = fake(id, 0x002a);
  const port = await DfuUsbPort.openBootloader(f.usb as never, id);
  assert.ok(f.history.some((x) => x.includes('"productId":42')));
  await port.close();
  assert.equal(f.closed, true);
  await assert.rejects(DfuUsbPort.openBootloader(fake(null, 0x002a).usb as never, id), /doğrulanamadı/);
});
