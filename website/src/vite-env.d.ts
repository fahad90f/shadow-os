interface ImportMetaEnv {
  /** Base URL of the package repository when it is not this site; see .env.example. */
  readonly VITE_REPO_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
