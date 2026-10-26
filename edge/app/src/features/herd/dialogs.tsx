// Short edits for an animal: identity, a dated note; and the add-collar sheet.
import { useState } from "react";
import { shortId } from "../../app/herd.ts";
import { tr } from "../../app/i18n/tr.ts";
import type { Animal, Sex } from "../../app/model.ts";
import { useBack } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { SWATCHES } from "../../app/stores.ts";
import { Dialog, Seg } from "../../ds/components.tsx";

export function EditAnimal({ a, onClose }: { a: Animal; onClose: () => void }) {
  const m = useApp();
  useBack(onClose);
  const [f, setF] = useState(() => ({ ...a.profile }));
  const short = shortId(a.device ?? a.id);
  const save = () => { m.actions.saveAsset(f, a.device); onClose(); };
  return (
    <Dialog title={tr.edit.title} onClose={onClose}>
      <label className="lab">{tr.animal.fields.name}
        <input className="inp" value={f.name} placeholder={tr.edit.namePlaceholder(short)} onChange={(e) => setF({ ...f, name: e.target.value })} />
      </label>
      <label className="lab">{tr.animal.fields.tag}
        <input className="inp" value={f.tag} placeholder={tr.edit.tagPlaceholder} autoCapitalize="characters" onChange={(e) => setF({ ...f, tag: e.target.value })} />
      </label>
      <label className="lab">{tr.animal.fields.breed}
        <input className="inp" value={f.breed} placeholder={tr.edit.breedPlaceholder} onChange={(e) => setF({ ...f, breed: e.target.value })} />
      </label>
      <div className="lab">{tr.animal.fields.sex}
        <Seg<Sex> value={f.sex} onChange={(v) => setF({ ...f, sex: v })}
          options={[{ value: "female", label: tr.animal.sex.female }, { value: "male", label: tr.animal.sex.male }, { value: "", label: tr.animal.sex[""] }]} />
      </div>
      <label className="lab">{tr.animal.fields.birth}
        <input className="inp" type="date" value={f.birth} max={new Date().toISOString().slice(0, 10)} onChange={(e) => setF({ ...f, birth: e.target.value })} />
      </label>
      <div className="lab">{tr.animal.fields.color}
        <div className="swatches">
          {SWATCHES.map((c) => (
            <button key={c} className="swatch" style={{ background: c }} aria-pressed={c === f.color} aria-label={tr.edit.swatch} onClick={() => setF({ ...f, color: c })} />
          ))}
        </div>
      </div>
      <div className="btns"><button className="btn" onClick={onClose}>{tr.common.cancel}</button><button className="btn primary" onClick={save}>{tr.common.save}</button></div>
    </Dialog>
  );
}

export function AddNote({ a, onClose }: { a: Animal; onClose: () => void }) {
  const m = useApp();
  useBack(onClose);
  const [text, setText] = useState("");
  return (
    <Dialog title={tr.animal.addNote} onClose={onClose}>
      <label className="lab">{a.name}
        <textarea className="area" autoFocus rows={4} value={text} placeholder={tr.animal.notePlaceholder} onChange={(e) => setText(e.target.value)} />
      </label>
      <div className="btns">
        <button className="btn" onClick={onClose}>{tr.common.cancel}</button>
        <button className="btn primary" disabled={!text.trim()} onClick={() => { m.actions.addNote(a.id, a.device, text); onClose(); }}>{tr.common.save}</button>
      </div>
    </Dialog>
  );
}

export function AddDevice({ onClose }: { onClose: () => void }) {
  useBack(onClose);
  return (
    <Dialog title={tr.add.title} onClose={onClose}>
      <p style={{ padding: "0 20px", margin: 0 }}>{tr.add.body}</p>
      <p className="sm" style={{ padding: "12px 20px 0", margin: 0 }}>{tr.add.after}</p>
      <div className="btns"><button className="btn primary" onClick={onClose}>{tr.common.ok}</button></div>
    </Dialog>
  );
}
