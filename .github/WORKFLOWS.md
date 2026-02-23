# GitHub Actions Workflows

This project uses GitHub Actions for continuous integration and deployment.

## Workflows

### Build and Release (`build.yml`)

**Triggers:**
- Push to `main` or `master` branch
- Pull requests to `main` or `master`
- Release creation
- Manual dispatch via Actions tab

**What it does:**
1. **Builds** the project on Windows using Visual Studio 2022
2. **Creates artifacts** for both Debug and Release configurations
3. **Uploads artifacts** to GitHub (available for 90 days)
4. **Packages release** when a GitHub release is created

**Artifacts include:**
- `useeplus_camera.dll` - Main driver library
- `useeplus_camera.lib` - Import library for linking
- `camera_capture.exe` - Simple frame capture tool
- `live_viewer.exe` - GDI+ based viewer
- `live_viewer_imgui.exe` - Advanced viewer with ImGui controls
- `diagnostic.exe` - USB device diagnostics
- `simple_winusb_test.exe` - WinUSB testing tool
- All headers, docs, and license files

## Downloading Build Artifacts

### From Actions Tab

1. Go to the [Actions](https://github.com/YOUR_USERNAME/useeplus-linux-driver/actions) tab
2. Click on a workflow run (green checkmark = success)
3. Scroll to "Artifacts" section at the bottom
4. Download `useeplus-camera-Release-{commit-sha}`
5. Extract and use!

### From Releases

If a release has been created:
1. Go to [Releases](https://github.com/YOUR_USERNAME/useeplus-linux-driver/releases)
2. Download `useeplus-camera-windows-x64.zip` or `.tar.gz`
3. Extract and use!

## Creating a Release

To trigger an official release with binaries:

1. **Tag the commit:**
   ```bash
   git tag -a v1.0.0 -m "Release version 1.0.0"
   git push origin v1.0.0
   ```

2. **Create GitHub Release:**
   - Go to repository → Releases → "Draft a new release"
   - Choose the tag you just created
   - Fill in release title and description
   - Publish the release

3. **Automatic build:**
   - The workflow will automatically trigger
   - Binaries will be built and attached to the release
   - Both `.zip` and `.tar.gz` formats will be available

## Manual Workflow Trigger

You can manually trigger a build:
1. Go to Actions tab
2. Select "Build and Release" workflow
3. Click "Run workflow" button
4. Choose branch and click "Run workflow"

## Build Status Badge

Add this to your README to show build status:

```markdown
![Build Status](https://github.com/YOUR_USERNAME/useeplus-linux-driver/workflows/Build%20and%20Release/badge.svg)
```

Replace `YOUR_USERNAME` with your GitHub username.

## Workflow Configuration

The workflow is defined in `.github/workflows/build.yml`.

Key features:
- **Matrix build:** Builds both Debug and Release in parallel
- **Caching:** CMake dependencies (ImGui) are cached
- **Retention:** Artifacts kept for 90 days
- **Cross-platform ready:** Can easily add Linux/Mac builds if needed

## Troubleshooting

**Build fails:**
- Check the Actions tab for error logs
- Common issues: CMake configuration, missing dependencies
- Ensure all source files are committed

**Artifacts not appearing:**
- Check if the build step succeeded
- Verify the file paths in the workflow match your build output

**Release assets not attaching:**
- Make sure you created a GitHub Release (not just a tag)
- Check that workflow has permission to write to releases
- Verify GITHUB_TOKEN is available (should be automatic)

## Modifying the Workflow

To modify the build process:
1. Edit `.github/workflows/build.yml`
2. Test locally if possible
3. Commit and push to see results in Actions tab
4. Check the workflow run for any errors

**Important files:**
- `.github/workflows/build.yml` - Main workflow definition
- `CMakeLists.txt` - Build configuration (affects what gets built)

## Security

- Workflows run in isolated environments
- Only trusted contributors should have write access
- Review workflow changes carefully in pull requests
- Never commit secrets or credentials to the repository

## Resources

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Workflow Syntax](https://docs.github.com/en/actions/using-workflows/workflow-syntax-for-github-actions)
- [Upload Artifacts Action](https://github.com/actions/upload-artifact)
