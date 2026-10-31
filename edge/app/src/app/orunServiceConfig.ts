// ORUN account API origin: one central deployment setting, not a per-phone form.
// During development access is provided by private Tailscale HTTPS Serve.
// Switching to an owned domain requires changing only the build/deploy config.
// This constant is a PUBLIC endpoint, never a credential or API token.
export const ORUN_ACCOUNT_ORIGIN: string =
  import.meta.env.VITE_ORUN_ACCOUNT_ORIGIN ||
  "https://simsek.tail823510.ts.net:8443";
