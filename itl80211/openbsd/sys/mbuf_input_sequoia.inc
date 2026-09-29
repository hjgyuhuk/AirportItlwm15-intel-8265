extern UInt32 itlwmNativeInputPacket(IOEthernetController *, mbuf_t);

static IOReturn _if_input(OSObject *, void *arg0, void *arg1, void *, void *)
{
    auto *ifp = static_cast<_ifnet *>(arg0);
    auto *list = static_cast<mbuf_list *>(arg1);
    mbuf_t packet;
    // Native inputPacket consumes/frees the mbuf immediately. Remove it from
    // the list first; never obtain nextpkt from a transferred packet.
    while ((packet = ml_dequeue(list)) != nullptr) {
        UInt32 accepted;
        if (ifp->iface) {
            accepted = ifp->iface->inputPacket(packet, 0);
        } else {
            accepted = itlwmNativeInputPacket(ifp->controller, packet);
            if (!accepted) mbuf_freem(packet);
        }
        if (accepted && ifp->netStat) ++ifp->netStat->inputPackets;
    }
    return kIOReturnSuccess;
}
