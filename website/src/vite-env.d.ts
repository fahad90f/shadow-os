interface ImportMetaEnv {
  /** Base URL of the package repository; see .env.example. */
  readonly VITE_REPO_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
