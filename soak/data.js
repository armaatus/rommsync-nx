window.BENCHMARK_DATA = {
  "lastUpdate": 1792917362270,
  "repoUrl": "https://github.com/armaatus/rommsync-nx",
  "entries": {
    "soak": [
      {
        "commit": {
          "author": {
            "email": "jorisxdejong@gmail.com",
            "name": "Joris de Jong",
            "username": "armaatus"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "6e35a70bad8420d239fcb8d9947ac142e7bd45ca",
          "message": "Merge pull request #274 from armaatus/fleet/m11",
          "timestamp": "2026-10-23T18:33:47+02:00",
          "tree_id": "3c4623f481c7728ca17684ab38908f6b2e4a0031",
          "url": "https://github.com/armaatus/rommsync-nx/commit/6e35a70bad8420d239fcb8d9947ac142e7bd45ca"
        },
        "date": 1792774098791,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "soak: held between rounds",
            "value": 67303,
            "unit": "bytes"
          },
          {
            "name": "soak: a round's peak over that",
            "value": 51800,
            "unit": "bytes"
          },
          {
            "name": "soak: allocations a round",
            "value": 3545,
            "unit": "allocations"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Joris de Jong",
            "username": "armaatus",
            "email": "jorisxdejong@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "59ddb11816343be251902db2bdfd8a15cd1e351c",
          "message": "Merge pull request #278 from armaatus/fleet/m13\n\nM13: drop the vendored autofleet; hold Keep local under a running game",
          "timestamp": "2026-10-23T23:09:22Z",
          "url": "https://github.com/armaatus/rommsync-nx/commit/59ddb11816343be251902db2bdfd8a15cd1e351c"
        },
        "date": 1792829849227,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "soak: held between rounds",
            "value": 67303,
            "unit": "bytes"
          },
          {
            "name": "soak: a round's peak over that",
            "value": 53809,
            "unit": "bytes"
          },
          {
            "name": "soak: allocations a round",
            "value": 3516,
            "unit": "allocations"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "name": "Joris de Jong",
            "username": "armaatus",
            "email": "jorisxdejong@gmail.com"
          },
          "committer": {
            "name": "GitHub",
            "username": "web-flow",
            "email": "noreply@github.com"
          },
          "id": "9575cc29df5c607bd7738642cb75ed6b2efd58a8",
          "message": "M14: cancel a held Keep local; guard.py judges shell writes (#280)",
          "timestamp": "2026-10-24T10:53:56Z",
          "url": "https://github.com/armaatus/rommsync-nx/commit/9575cc29df5c607bd7738642cb75ed6b2efd58a8"
        },
        "date": 1792917361747,
        "tool": "customSmallerIsBetter",
        "benches": [
          {
            "name": "soak: held between rounds",
            "value": 67303,
            "unit": "bytes"
          },
          {
            "name": "soak: a round's peak over that",
            "value": 51800,
            "unit": "bytes"
          },
          {
            "name": "soak: allocations a round",
            "value": 3539,
            "unit": "allocations"
          }
        ]
      }
    ]
  }
}