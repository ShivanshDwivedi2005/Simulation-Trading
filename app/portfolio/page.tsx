"use client";

import TradingTerminal from "@/components/trading-terminal";
import { useRouter } from "next/navigation";
import { useEffect, useMemo, useSyncExternalStore } from "react";

type Session = { email: string; name: string; token: string };

function subscribeToSession(onStoreChange: () => void) {
  window.addEventListener("storage", onStoreChange);
  return () => window.removeEventListener("storage", onStoreChange);
}

function getSessionSnapshot() {
  return localStorage.getItem("simtrade_session");
}

export default function PortfolioPage() {
  const router = useRouter();
  const storedSession = useSyncExternalStore(subscribeToSession, getSessionSnapshot, () => null);
  const session = useMemo(() => {
    if (!storedSession) return null;
    try {
      const value = JSON.parse(storedSession) as Partial<Session>;
      return value.email && value.name && value.token ? value as Session : null;
    } catch {
      return null;
    }
  }, [storedSession]);

  useEffect(() => {
    if (!session) {
      if (storedSession) localStorage.removeItem("simtrade_session");
      router.replace("/?auth=login");
    }
  }, [router, session, storedSession]);

  function signOut() {
    if (!window.confirm("Do you want to log out?")) return;
    localStorage.removeItem("simtrade_session");
    router.replace("/");
  }

  if (!session) {
    return <main className="auth-loading"><span className="brand-mark" aria-hidden="true">ST</span><p>Securing your workspace…</p></main>;
  }

  return <TradingTerminal userName={session.name} accessToken={session.token} onSignOut={signOut} />;
}
