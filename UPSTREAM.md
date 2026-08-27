# Upstream VKMS baseline

CastKMS is derived from VKMS at this baseline:

- Repository: https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
- Tag: `v7.2`
- Commit: `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`
- Original path: `drivers/gpu/drm/vkms`

The repository keeps the complete Linux history leading to this release. The
standalone tree is pruned from the source at the commit immediately after the
baseline tag.

That provenance makes a full clone much larger than the standalone source
tree. Contributors who do not need to inspect pre-CastKMS kernel history can
start with a shallow clone and deepen it later if necessary:

```sh
git clone --depth=1 <repository-url> castkms
git -C castkms fetch --deepen=100
```
