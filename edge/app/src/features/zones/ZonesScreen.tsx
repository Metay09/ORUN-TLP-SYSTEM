// Alanlar: the pasture boundaries the collars check by themselves.
import { Polygon } from "@phosphor-icons/react";
import { tr } from "../../app/i18n/tr.ts";
import { useApp } from "../../app/store.ts";
import { DemoFlag, Empty, Header } from "../../ds/components.tsx";

export function ZonesScreen() {
  const m = useApp();
  return (
    <div className="page">
      <Header title={tr.zones.title} />
      {m.demo && <DemoFlag onPage />}
      <Empty icon={<Polygon size={34} />} title={tr.zones.empty} body={tr.zones.emptyBody}>
        <span className="fine">{tr.zones.rules}</span>
      </Empty>
    </div>
  );
}
