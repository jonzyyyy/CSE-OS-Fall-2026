# CSE 4300 Group Project
Members: Jonathan, Jose, Rodge, Samuel, Zakir
Topic: Multithreaded chat client-server

## Team workflow

1. Work on a branch in the team fork (`jonzyyyy/CSE-OS-Fall-2026`) and open a PR into the fork's `main`.
2. The **Tests** check runs automatically on every PR. Merge only when it's green.

## Submitting to the class repo

**Never use GitHub's "Contribute → Open pull request" button, and never open a PR to `cloud-sys-lab` by hand.**
That would also send the fork's `.github/` folder to the class repo.

Instead:

1. Fork → **Actions** tab → **Submit to class repo** → **Run workflow**.
2. Enter a name (e.g. `worker-tests`) and click **Run workflow**.
3. When it turns green, open the run and click **Open PR to class repo →**.
4. Check that **Files changed** only lists files in this folder, then click **Create pull request**.

## Testing

See [tests/README.md](tests/README.md). Run `make test` from this folder.
