#ifndef MIME_H
#define MIME_H

// 파일 경로의 확장자로 Content-Type을 정한다. 모르면 "application/octet-stream"
const char *mime_from_path(const char *path);

#endif
