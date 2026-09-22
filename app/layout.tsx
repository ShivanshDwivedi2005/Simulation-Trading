import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "SimTrade | US Market Simulation",
  description: "Practice realistic US-market trading with virtual cash, live-style quotes, risk controls, and portfolio analytics.",
  icons: {
    icon: "/favicon.svg",
    shortcut: "/favicon.svg",
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body className="antialiased">{children}</body>
    </html>
  );
}
