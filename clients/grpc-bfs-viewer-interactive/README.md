# CPGDB interactive gRPC-Web BFS Viewer

Interactive BFS viewer client for CPGDB using the gRPC backend

## Install

Put this folder at:

```text
web/grpc-bfs-viewer
```

then:

```bash
cd clients/grpc-bfs-viewer
npm install
npm run gen
npm run dev
```

The generator expects:

```text
../../proto/bfs_stream.proto
```

The browser should use:

```text
http://localhost:8081
```

as the gRPC-Web URL.

## If you still see end-of-stream transport errors

If the graph appears and only the final status fails, the frontend path is working. Check that the C++ server returns `::grpc::Status::OK` after the streaming loop and that Envoy does not close the stream before gRPC trailers are sent.
