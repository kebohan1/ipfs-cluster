// Package single implements a ClusterDAGService that chunks and adds content
// to cluster without sharding, before pinning it.
package single

import (
	"context"

	adder "github.com/kebohan1/ipfs-cluster/adder"
	"github.com/kebohan1/ipfs-cluster/api"

	cid "github.com/ipfs/go-cid"
	ipld "github.com/ipfs/go-ipld-format"
	logging "github.com/ipfs/go-log/v2"
	peer "github.com/libp2p/go-libp2p-core/peer"
	rpc "github.com/libp2p/go-libp2p-gorpc"
)

var logger = logging.Logger("singledags")
var _ = logger // otherwise unused

// DAGService is an implementation of an adder.ClusterDAGService which
// puts the added blocks directly in the peers allocated to them (without
// sharding).
type DAGService struct {
	adder.BaseDAGService

	rpcClient *rpc.Client

	dests   []peer.ID
	pinOpts api.PinOptions
	local   bool

	ba *adder.BlockAdder
}

// New returns a new Adder with the given rpc Client. The client is used
// to perform calls to IPFS.BlockPut and Pin content on Cluster.
func New(rpc *rpc.Client, opts api.PinOptions, local bool) *DAGService {
	// ensure don't Add something and pin it in direct mode.
	opts.Mode = api.PinModeRecursive
	return &DAGService{
		rpcClient: rpc,
		dests:     nil,
		pinOpts:   opts,
		local:     local,
	}
}

// Add puts the given node in the destination peers.
func (dgs *DAGService) Add(ctx context.Context, node ipld.Node) error {
	if dgs.dests == nil {
		dests, err := adder.BlockAllocate(ctx, dgs.rpcClient, dgs.pinOpts)
		if err != nil {
			return err
		}
		dgs.dests = dests

		if dgs.local {
			dgs.ba = adder.NewBlockAdder(dgs.rpcClient, []peer.ID{""})
		} else {
			dgs.ba = adder.NewBlockAdder(dgs.rpcClient, dests)
		}
	}

	return dgs.ba.Add(ctx, node)
}

// Finalize pins the last Cid added to this DAGService.
func (dgs *DAGService) Finalize(ctx context.Context, root cid.Cid) (cid.Cid, error) {
	// Cluster pin the result
	rootPin := api.PinWithOpts(root, dgs.pinOpts)
	rootPin.Allocations = dgs.dests
	dgs.dests = nil

	return root, adder.Pin(ctx, dgs.rpcClient, rootPin)
}

// AddMany calls Add for every given node.
func (dgs *DAGService) AddMany(ctx context.Context, nodes []ipld.Node) error {
	for _, node := range nodes {
		err := dgs.Add(ctx, node)
		if err != nil {
			return err
		}
	}
	return nil
}
