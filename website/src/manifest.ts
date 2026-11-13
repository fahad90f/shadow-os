import { useEffect, useState } from "react";

/** Base URL of the package repository; CI publishes repo.json next to it. */
export const REPO_URL = (import.meta.env.VITE_REPO_URL ?? "https://fahad90f.github.io/shadow-os").replace(/\/+$/, "");

export interface RepoPackage {
  name: string;
  version: string;
  summary: string;
  /** Size of the .deb in bytes. */
  size: number;
  /** Path of the .deb below the repository URL. */
  filename: string;
  component: string;
  architecture: string;
}

/** repo.json, written by scripts/gen-repo-site.py when a build is published. */
export interface RepoManifest {
  schema: 1;
  name: string;
  version: string;
  codename: string;
  channel: string;
  /** Package version of the published build, e.g. "0.2.0+build48". */
  build: string;
  /** Publication time, ISO 8601 in UTC. */
  published: string;
  commit: string;
  sourceUrl: string;
  /** Fingerprint of the key that signs the repository; empty if unknown. */
  fingerprint: string;
  architectures: string[];
  packages: RepoPackage[];
}

export type ManifestState =
  | { status: "loading" }
  | { status: "ready"; manifest: RepoManifest }
  | { status: "unpublished" };

function isManifest(value: unknown): value is RepoManifest {
  if (typeof value !== "object" || value === null) return false;
  const candidate = value as Partial<RepoManifest>;
  return candidate.schema === 1 && typeof candidate.build === "string" && Array.isArray(candidate.packages);
}

export function useManifest(): ManifestState {
  const [state, setState] = useState<ManifestState>({ status: "loading" });

  useEffect(() => {
    const controller = new AbortController();
    fetch(`${REPO_URL}/repo.json`, { signal: controller.signal, cache: "no-cache" })
      .then((response) => (response.ok ? response.json() : null))
      .then((body: unknown) => {
        setState(isManifest(body) ? { status: "ready", manifest: body } : { status: "unpublished" });
      })
      .catch((error: unknown) => {
        if (!(error instanceof DOMException && error.name === "AbortError")) setState({ status: "unpublished" });
      });
    return () => controller.abort();
  }, []);

  return state;
}

export function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

/** "2026-11-13T05:04:11Z" → "2026-11-13 05:04"; the input unchanged if it is not a date. */
export function formatPublished(iso: string): string {
  const date = new Date(iso);
  return Number.isNaN(date.getTime()) ? iso : date.toISOString().slice(0, 16).replace("T", " ");
}

export function formatFingerprint(fingerprint: string): string {
  return fingerprint.match(/.{1,4}/g)?.join(" ") ?? fingerprint;
}
