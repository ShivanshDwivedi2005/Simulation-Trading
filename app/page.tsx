"use client";

import {
  Activity,
  ArrowLeft,
  ArrowRight,
  BarChart3,
  Check,
  LineChart,
  LockKeyhole,
  Pause,
  Play,
  ShieldCheck,
  Sparkles,
} from "lucide-react";
import { useRouter, useSearchParams } from "next/navigation";
import { FormEvent, Suspense, useEffect, useRef, useState } from "react";

type AuthMode = "login" | "signup" | "verify";

type SessionResponse = {
  access_token?: string;
  user?: { email: string; name: string };
  message?: string;
};

const API_URL = process.env.NEXT_PUBLIC_API_URL ?? "http://localhost:8080";

const VIDEO_URL = "https://d33g0704mc6hny.cloudfront.net/technology_page_banner_video.mp4";

function HomeContent() {
  const router = useRouter();
  const searchParams = useSearchParams();
  const videoRef = useRef<HTMLVideoElement>(null);
  const authQuery = searchParams.get("auth");
  const routeAuthMode: Exclude<AuthMode, "verify"> | null = authQuery === "login" || authQuery === "signup" ? authQuery : null;
  const [authStep, setAuthStep] = useState<"verify" | null>(null);
  const authMode: AuthMode | null = authStep ?? routeAuthMode;
  const [videoPaused, setVideoPaused] = useState(() => typeof window !== "undefined" && window.matchMedia("(prefers-reduced-motion: reduce)").matches);
  const [formError, setFormError] = useState("");
  const [formNotice, setFormNotice] = useState("");
  const [verificationEmail, setVerificationEmail] = useState("");
  const [loginEmail, setLoginEmail] = useState("");
  const [submitting, setSubmitting] = useState(false);

  useEffect(() => {
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) {
      videoRef.current?.pause();
    }
  }, []);

  function openAuth(mode: AuthMode) {
    setFormError("");
    setFormNotice("");
    setSubmitting(false);
    if (mode === "verify") {
      setAuthStep("verify");
      return;
    }

    setAuthStep(null);
    const href = `/?auth=${mode}`;
    if (authQuery === "login" || authQuery === "signup") {
      router.replace(href, { scroll: false });
    } else {
      router.push(href, { scroll: false });
    }
  }

  function returnHome() {
    setAuthStep(null);
    router.replace("/", { scroll: false });
  }

  function toggleVideo() {
    const video = videoRef.current;
    if (!video) return;
    if (video.paused) {
      void video.play();
      setVideoPaused(false);
    } else {
      video.pause();
      setVideoPaused(true);
    }
  }

  async function submitAuth(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const data = new FormData(event.currentTarget);
    const email = authMode === "verify" ? verificationEmail : String(data.get("email") ?? "").trim();
    const password = String(data.get("password") ?? "");
    const fullName = String(data.get("fullName") ?? "").trim();
    const otp = String(data.get("otp") ?? "").trim();

    if (!email.includes("@")) {
      setFormError("Enter a valid email address.");
      return;
    }
    if (authMode === "verify" && !/^\d{6}$/.test(otp)) {
      setFormError("Enter the 6-digit verification code.");
      return;
    }
    if (authMode !== "verify" && password.length < 8) {
      setFormError("Password must be at least 8 characters.");
      return;
    }
    if (authMode === "signup" && fullName.length < 2) {
      setFormError("Enter your full name.");
      return;
    }

    setSubmitting(true);
    setFormError("");
    setFormNotice("");
    try {
      const endpoint = authMode === "signup"
        ? "/api/v1/auth/register"
        : authMode === "verify"
          ? "/api/v1/auth/verify-otp"
          : "/api/v1/auth/login";
      const body = authMode === "signup"
        ? { name: fullName, email, password }
        : authMode === "verify"
          ? { email, otp }
          : { email, password };
      const response = await fetch(`${API_URL}${endpoint}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const result = await response.json().catch(() => ({})) as SessionResponse;
      if (!response.ok) throw new Error(result.message ?? "The request could not be completed. Please try again.");

      if (authMode === "signup") {
        setVerificationEmail(email);
        setAuthStep("verify");
        return;
      }

      if (authMode === "verify") {
        setLoginEmail(email);
        setVerificationEmail("");
        setFormNotice(result.message ?? "Email verified. Sign in to continue.");
        setAuthStep(null);
        router.replace("/?auth=login", { scroll: false });
        return;
      }

      if (!result.user || !result.access_token) {
        throw new Error("The sign-in response was incomplete. Please try again.");
      }

      localStorage.setItem("simtrade_session", JSON.stringify({
        email: result.user.email,
        name: result.user.name,
        token: result.access_token,
      }));
      router.replace("/portfolio");
    } catch (error) {
      setFormError(error instanceof Error ? error.message : "Unable to reach the backend API.");
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <main className="landing-shell">
      <video
        ref={videoRef}
        className="landing-video"
        src={VIDEO_URL}
        autoPlay
        muted
        loop
        playsInline
        preload="metadata"
        aria-hidden="true"
      />
      <div className="landing-scrim" aria-hidden="true" />

      <section className="landing-hero" aria-labelledby="hero-title">
        <header className="landing-nav">
          <a className="landing-brand" href="#top" aria-label="SimTrade home">
            <span className="brand-mark" aria-hidden="true"><Activity /></span>
            <span><strong>SIMTRADE</strong><small>Practice the market</small></span>
          </a>
          <nav aria-label="Main navigation">
            <a href="#platform">Platform</a>
            <a href="#why-simtrade">Why SimTrade</a>
          </nav>
          <div className="landing-nav-actions">
            <button className="landing-login" onClick={() => openAuth("login")}>Log in</button>
            <button className="landing-signup" onClick={() => openAuth("signup")}>Create account <ArrowRight aria-hidden="true" /></button>
          </div>
        </header>

        <div id="top" className="hero-content">
          <div className="hero-kicker"><Sparkles aria-hidden="true" /> Markets move fast. Your learning should too.</div>
          <h1 id="hero-title">Build conviction<br />before you risk capital.</h1>
          <p>Practice realistic US-market trading with virtual cash, live-style execution, and portfolio analytics built to sharpen every decision.</p>
          <div className="hero-actions">
            <button className="hero-primary" onClick={() => openAuth("signup")}>Start trading free <ArrowRight aria-hidden="true" /></button>
            <button className="hero-secondary" onClick={() => openAuth("login")}>I already have an account</button>
          </div>
          <ul className="hero-assurances" aria-label="Platform benefits">
            <li><Check aria-hidden="true" /> $100,000 virtual buying power</li>
            <li><Check aria-hidden="true" /> No real money at risk</li>
            <li><Check aria-hidden="true" /> Trade-ready in seconds</li>
          </ul>
        </div>

        <div className="market-glance" aria-label="Sample market prices">
          <span><b>NASDAQ</b><strong className="positive">+1.24%</strong></span>
          <span><b>AAPL</b><strong>227.16</strong><em className="positive">+1.42%</em></span>
          <span><b>NVDA</b><strong>141.22</strong><em className="positive">+2.84%</em></span>
          <span><b>MSFT</b><strong>515.73</strong><em className="negative">−0.38%</em></span>
        </div>

        <button className="video-control" onClick={toggleVideo} aria-label={videoPaused ? "Play background video" : "Pause background video"}>
          {videoPaused ? <Play aria-hidden="true" /> : <Pause aria-hidden="true" />}
          <span>{videoPaused ? "Play motion" : "Pause motion"}</span>
        </button>
      </section>

      <section id="platform" className="landing-section">
        <div className="section-heading">
          <span>THE PRACTICE ENVIRONMENT</span>
          <h2>Everything you need to trade the market—except the risk.</h2>
          <p>Move from market observation to confident execution in one focused workspace.</p>
        </div>
        <div className="feature-grid">
          <article><span><LineChart aria-hidden="true" /></span><h3>Read the market</h3><p>Study responsive candlestick, line, and area charts across the timeframes traders use every day.</p></article>
          <article><span><BarChart3 aria-hidden="true" /></span><h3>Practice execution</h3><p>Place market, limit, and stop orders with bid/ask-aware simulated fills and clear feedback.</p></article>
          <article><span><ShieldCheck aria-hidden="true" /></span><h3>Learn your risk</h3><p>Track buying power, positions, and unrealized P&amp;L without putting real capital on the line.</p></article>
        </div>
      </section>

      <section id="why-simtrade" className="landing-cta">
        <div><span>YOUR MARKET. YOUR PACE.</span><h2>Practice today. Trade with clarity tomorrow.</h2></div>
        <button className="hero-primary" onClick={() => openAuth("signup")}>Create free account <ArrowRight aria-hidden="true" /></button>
      </section>

      <footer className="landing-footer"><span>© 2026 SimTrade</span><span>Simulation only · No real funds are used</span></footer>

      {authMode && (
        <div className="auth-overlay">
          <section className="auth-dialog" role="dialog" aria-modal="true" aria-labelledby="auth-title">
            <button className="auth-back" onClick={returnHome}><ArrowLeft aria-hidden="true" /> Back to home</button>
            <div className="auth-icon" aria-hidden="true"><LockKeyhole /></div>
            <span className="auth-eyebrow">SIMTRADE ACCESS</span>
            <h2 id="auth-title">{authMode === "signup" ? "Create your trading profile" : authMode === "verify" ? "Verify your email" : "Welcome back"}</h2>
            <p>{authMode === "signup" ? "Start with a virtual portfolio and learn at your own pace." : authMode === "verify" ? `Enter the code sent to ${verificationEmail}.` : "Log in to continue to your simulated portfolio."}</p>
            <form key={authMode} onSubmit={submitAuth} noValidate>
              {authMode === "signup" && <label>Full name<input name="fullName" autoComplete="name" autoFocus placeholder="Alex Morgan" /></label>}
              {authMode !== "verify" && <label>Email address<input name="email" type="email" autoComplete="email" autoFocus={authMode === "login"} defaultValue={authMode === "login" ? loginEmail : ""} placeholder="you@example.com" /></label>}
              {authMode !== "verify" && <label>Password<input name="password" type="password" autoComplete={authMode === "signup" ? "new-password" : "current-password"} placeholder="Minimum 8 characters" /></label>}
              {authMode === "verify" && <label>Verification code<input name="otp" inputMode="numeric" autoComplete="one-time-code" autoFocus maxLength={6} pattern="[0-9]{6}" placeholder="000000" /></label>}
              {formNotice && <p className="auth-success" role="status">{formNotice}</p>}
              {formError && <p className="auth-error" role="alert">{formError}</p>}
              <button className="auth-submit" type="submit" disabled={submitting}>{submitting ? "Please wait…" : authMode === "signup" ? "Create account" : authMode === "verify" ? "Verify email" : "Log in"}<ArrowRight aria-hidden="true" /></button>
            </form>
            {authMode === "verify" ? <p className="auth-switch">Wrong email? <button onClick={() => openAuth("signup")}>Start again</button></p> : <p className="auth-switch">{authMode === "signup" ? "Already have an account?" : "New to SimTrade?"} <button onClick={() => openAuth(authMode === "signup" ? "login" : "signup")}>{authMode === "signup" ? "Log in" : "Create one"}</button></p>}
            <small className="auth-disclaimer">Authentication is handled by the SimTrade API. Brokerage credentials are never requested.</small>
          </section>
        </div>
      )}
    </main>
  );
}

export default function Home() {
  return <Suspense fallback={null}><HomeContent /></Suspense>;
}
