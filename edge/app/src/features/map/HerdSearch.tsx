// "Hayvan ara" over the map: type a name, ear tag or collar; pick to select.
import { useState } from "react";
import { MagnifyingGlass } from "@phosphor-icons/react";
import { matches } from "../../app/herd.ts";
import { tr } from "../../app/i18n/tr.ts";
import type { Animal } from "../../app/model.ts";
import { CaretLeft } from "@phosphor-icons/react";
import { AnimalRing, toneOfStatus } from "../../ds/components.tsx";

export function HerdSearch({ herd, onClose, onPick }: { herd: Animal[]; onClose: () => void; onPick: (id: string) => void }) {
  const [q, setQ] = useState("");
  const list = herd.filter((a) => matches(a, q));
  return (
    <div className="find-list" role="dialog" aria-label={tr.map.search}>
      <div className="hdr">
        <button className="circ" onClick={onClose} aria-label={tr.common.back}><CaretLeft size={22} weight="bold" /></button>
        <div className="search grow" style={{ padding: 0 }}>
          <MagnifyingGlass size={20} style={{ left: 14 }} />
          <input className="inp" type="search" autoFocus value={q} placeholder={tr.herd.search} aria-label={tr.map.search} onChange={(e) => setQ(e.target.value)} />
        </div>
      </div>
      <div className="page">
        {list.map((a) => (
          <button key={a.id} className="cell" onClick={() => onPick(a.id)}>
            <AnimalRing a={a} size={44} />
            <span className="cm">
              <span className="ct">{a.name}{a.profile.tag && <span className="cap" style={{ fontWeight: 400 }}> · {a.profile.tag}</span>}</span>
              <span className={`cs strong t-${toneOfStatus(a.status)}`}>{a.seen}</span>
            </span>
          </button>
        ))}
        {!list.length && <p className="sm" style={{ padding: "24px 20px" }}>{tr.herd.noMatch}</p>}
      </div>
    </div>
  );
}
