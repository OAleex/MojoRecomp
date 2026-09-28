#pragma once

#include <string>

void VfsSetGameRoot(const std::string& hostPath);
void VfsSetGameOverlay(const std::string& hostPath);
void VfsSetUserRoot(const std::string& hostPath);
void VfsSetUserRoots(const std::string& savePath,
                     const std::string& contentPath,
                     const std::string& cachePath,
                     const std::string& utilityPath);
void VfsMountDevice(const std::string& device, const std::string& hostPath, bool writable);
void VfsUnmountDevice(const std::string& device);
bool VfsCreateSymbolicLink(const std::string& linkName, const std::string& targetPath);
bool VfsDeleteSymbolicLink(const std::string& linkName);

std::string VfsTranslate(const std::string& guestPath, bool forWrite = false);
std::string VfsResolveExisting(const std::string& guestPath);
void VfsForget(const std::string& guestPath);
bool VfsDeviceWritable(const std::string& guestPath);
